/*
 * deskflow_client.c - see deskflow_client.h.
 *
 * Wire format notes (from deskflow ProtocolUtil.cpp and PacketStreamFilter.cpp):
 *   - EVERY message is framed with a 4-byte big-endian length prefix:
 *       [uint32 length][payload]
 *     This framing is added by deskflow's PacketStreamFilter on both ends.
 *   - Within the payload, integers are big-endian, 1/2/4 bytes (%i / %2i / %4i).
 *   - Strings are length-prefixed: 4-byte big-endian length + bytes (%s);
 *     %1s / %2s use 1 / 2-byte length prefixes.
 *   - Every message payload begins with a 4-byte code.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "deskflow_client.h"
#include "clipboard.h"

#include <errno.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#define DF_PROTOCOL_MAJOR 1
#define DF_PROTOCOL_MINOR 8
#define DF_MAX_MESSAGE    (4 * 1024 * 1024) /* PROTOCOL_MAX_MESSAGE_LENGTH */

#define LOG_ERR(...)  fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_INFO(...) fprintf(stderr, "INFO:  " __VA_ARGS__)
#define LOG_DBG(...)  fprintf(stderr, "DEBUG: " __VA_ARGS__)

#ifdef _WIN32
typedef SOCKET df_socket;
#define DF_INVALID_SOCKET INVALID_SOCKET
#define df_close_socket closesocket

static int df_socket_init(void)
{
    WSADATA data;
    int r = WSAStartup(MAKEWORD(2, 2), &data);
    if (r != 0) {
        LOG_ERR("WSAStartup failed: %d\n", r);
    }
    return r == 0 ? 0 : -1;
}

static void df_socket_cleanup(void)
{
    WSACleanup();
}

static int df_socket_interrupted(void)
{
    return WSAGetLastError() == WSAEINTR;
}

static int df_socket_error(void) { return WSAGetLastError(); }

static int df_socket_wait_timeout(df_socket fd, int timeout_ms)
{
    WSAPOLLFD pfd = {0};
    pfd.fd = fd;
    pfd.events = POLLRDNORM;
    return WSAPoll(&pfd, 1, timeout_ms);
}
#else
typedef int df_socket;
#define DF_INVALID_SOCKET (-1)
#define df_close_socket close

static int df_socket_init(void) { return 0; }
static void df_socket_cleanup(void) { }
static int df_socket_interrupted(void) { return errno == EINTR; }
static int df_socket_error(void) { return errno; }

static int df_socket_wait_timeout(df_socket fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    return poll(&pfd, 1, timeout_ms);
}
#endif

static int io_wait_interval_ms = 500;

static int df_socket_wait_events(df_socket fd, short events, int timeout_ms)
{
#ifdef _WIN32
    WSAPOLLFD pfd = {0};
    pfd.fd = fd;
    pfd.events = events;
    return WSAPoll(&pfd, 1, timeout_ms);
#else
    struct pollfd pfd = { .fd = fd, .events = events };
    return poll(&pfd, 1, timeout_ms);
#endif
}

static int df_socket_set_nonblocking(df_socket fd, bool enabled)
{
#ifdef _WIN32
    u_long value = enabled ? 1UL : 0UL;
    return ioctlsocket(fd, FIONBIO, &value) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (enabled) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
#endif
}

static int df_socket_wait(df_socket fd)
{
    return df_socket_wait_timeout(fd, io_wait_interval_ms);
}

struct df_client {
    df_socket fd;
    int32_t screen_w;
    int32_t screen_h;
    uint16_t proto_minor;   /* negotiated protocol minor version */
    df_client_callbacks cbs;
    void *ud;
    int accepted;
    unsigned info_pending;
    bool resizing;
    bool clipboard_requested, clipboard_server, clipboard_active, clipboard_owned;
    uint32_t clipboard_seq;
    size_t clipboard_limit;
    uint8_t *clipboard_local;
    size_t clipboard_local_size;
    SSL_CTX *tls_ctx;
    SSL *tls_ssl;
    char *tls_fingerprint;
    char *tls_fingerprint_file;
    bool tls_enabled;
    struct {
        uint8_t *data;
        size_t size, expected;
        uint32_t sequence;
        bool active, drop;
    } clipboard_rx[2];
};

static int (*stop_check)(void);
static df_client *io_client;

static char *df_strdup(const char *text)
{
    if (!text) return NULL;
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy) memcpy(copy, text, length);
    return copy;
}

void df_client_set_stop_check(int (*check)(void))
{
    stop_check = check;
}

/* ---- low-level socket I/O ----------------------------------------------- */

static int socket_suppress_sigpipe(df_socket fd)
{
#ifdef SO_NOSIGPIPE
    /* Darwin uses a socket option instead of Linux's MSG_NOSIGNAL. */
    int enabled = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
    (void)fd;
    return 0;
#endif
}

/* Mouse motion is latency-sensitive.  Without this, Nagle can leave a burst
 * of tiny Deskflow packets waiting for an ACK while the USB side is already
 * processing an older cursor position. */
static void socket_enable_low_latency(df_socket fd)
{
    int enabled = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     (const char *)&enabled, sizeof(enabled));
}

#ifdef _WIN32
#define DF_POLL_READ  POLLRDNORM
#define DF_POLL_WRITE POLLWRNORM
#else
#define DF_POLL_READ  POLLIN
#define DF_POLL_WRITE POLLOUT
#endif

static void tls_log_error(const char *operation)
{
    unsigned long error = ERR_get_error();
    char message[256] = "unknown OpenSSL error";
    if (error) ERR_error_string_n(error, message, sizeof(message));
    LOG_ERR("TLS %s failed: %s\n", operation, message);
}

static int tls_wait(df_socket fd, short events)
{
    for (;;) {
        if (stop_check && stop_check()) return -1;
        int ready = df_socket_wait_events(fd, events, io_wait_interval_ms);
        if (ready > 0) return 0;
        if (ready < 0 && df_socket_interrupted()) continue;
        if (ready < 0) return -1;
        if (io_client && io_client->cbs.on_tick && io_client->cbs.on_tick(io_client->ud)) {
            return -1;
        }
    }
}

/* Accept both Deskflow's v2:sha256:... database line and the compact or
 * colon-separated form printed by its status UI. */
static int tls_parse_fingerprint(const char *text, uint8_t output[32])
{
    if (!text) return -1;
    const char *value = text;
    const char *prefix = strstr(value, "v2:sha256:");
    if (prefix) value = prefix + strlen("v2:sha256:");

    size_t digits = 0;
    int high = -1;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == ':' || *p == '-' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        if (!isxdigit(*p) || digits >= 64) return -1;
        int digit = *p <= '9' ? *p - '0' :
                    (*p <= 'F' ? *p - 'A' + 10 : *p - 'a' + 10);
        if (high < 0) high = digit;
        else {
            output[digits / 2] = (uint8_t)((high << 4) | digit);
            high = -1;
        }
        ++digits;
    }
    return digits == 64 && high < 0 ? 0 : -1;
}

static int tls_fingerprint_matches(const uint8_t actual[32], const char *text)
{
    uint8_t expected[32];
    return tls_parse_fingerprint(text, expected) == 0 &&
           CRYPTO_memcmp(actual, expected, sizeof(expected)) == 0;
}

static int tls_verify_peer_fingerprint(const df_client *client)
{
    X509 *certificate = SSL_get1_peer_certificate(client->tls_ssl);
    if (!certificate) {
        LOG_ERR("TLS server did not provide a certificate\n");
        return -1;
    }
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    int digest_ok = X509_digest(certificate, EVP_sha256(), digest, &digest_size);
    X509_free(certificate);
    if (!digest_ok || digest_size != 32) {
        LOG_ERR("could not calculate TLS server certificate fingerprint\n");
        return -1;
    }
    char fingerprint[65];
    for (unsigned i = 0; i < 32; ++i)
        snprintf(fingerprint + i * 2, 3, "%02x", (unsigned)digest[i]);
    fingerprint[64] = '\0';
    LOG_INFO("TLS server SHA-256 fingerprint: %s\n", fingerprint);

    if (client->tls_fingerprint && tls_fingerprint_matches(digest, client->tls_fingerprint)) {
        return 0;
    }
    if (client->tls_fingerprint_file) {
        FILE *file = fopen(client->tls_fingerprint_file, "r");
        if (!file) {
            LOG_ERR("could not open TLS fingerprint file '%s'\n", client->tls_fingerprint_file);
            return -1;
        }
        char line[512];
        while (fgets(line, sizeof(line), file)) {
            if (tls_fingerprint_matches(digest, line)) {
                fclose(file);
                return 0;
            }
        }
        fclose(file);
    }
    LOG_ERR("TLS server certificate fingerprint is not trusted\n");
    return -1;
}

static int tls_setup(df_client *client, const df_client_tls_options *options)
{
    if (!options || !options->enabled) return 0;
    if ((!options->fingerprint || !*options->fingerprint) &&
        (!options->fingerprint_file || !*options->fingerprint_file)) {
        LOG_ERR("TLS requires --tls-fingerprint or --tls-fingerprint-file\n");
        return -1;
    }
    client->tls_ctx = SSL_CTX_new(TLS_client_method());
    if (!client->tls_ctx) {
        tls_log_error("context creation");
        return -1;
    }
    if (!SSL_CTX_set_min_proto_version(client->tls_ctx, TLS1_2_VERSION)) {
        tls_log_error("setting minimum protocol version");
        return -1;
    }
    /* Deskflow authenticates self-signed peers by SHA-256 fingerprint. */
    SSL_CTX_set_verify(client->tls_ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_clear_mode(client->tls_ctx, SSL_MODE_AUTO_RETRY);

    if (options->certificate && *options->certificate) {
        if (SSL_CTX_use_certificate_chain_file(client->tls_ctx, options->certificate) != 1 ||
            SSL_CTX_use_PrivateKey_file(client->tls_ctx, options->certificate, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(client->tls_ctx) != 1) {
            tls_log_error("loading client certificate");
            return -1;
        }
    }
    client->tls_ssl = SSL_new(client->tls_ctx);
    if (!client->tls_ssl || SSL_set_fd(client->tls_ssl, (int)client->fd) != 1) {
        tls_log_error("creating connection");
        return -1;
    }
    client->tls_fingerprint = df_strdup(options->fingerprint);
    client->tls_fingerprint_file = df_strdup(options->fingerprint_file);
    if ((options->fingerprint && !client->tls_fingerprint) ||
        (options->fingerprint_file && !client->tls_fingerprint_file)) return -1;
    client->tls_enabled = true;
    return 0;
}

static int tls_handshake(df_client *client)
{
    if (!client->tls_enabled) return 0;
    if (df_socket_set_nonblocking(client->fd, true) != 0) {
        LOG_ERR("could not make TLS socket non-blocking\n");
        return -1;
    }
    io_wait_interval_ms = 100;
    for (;;) {
        if (stop_check && stop_check()) return -1;
        int result = SSL_connect(client->tls_ssl);
        if (result == 1) break;
        int error = SSL_get_error(client->tls_ssl, result);
        if (error == SSL_ERROR_WANT_READ) {
            if (tls_wait(client->fd, DF_POLL_READ) != 0) return -1;
        } else if (error == SSL_ERROR_WANT_WRITE) {
            if (tls_wait(client->fd, DF_POLL_WRITE) != 0) return -1;
        } else {
            tls_log_error("handshake");
            return -1;
        }
    }
    if (tls_verify_peer_fingerprint(client) != 0) return -1;
    LOG_INFO("TLS connected (%s)\n", SSL_get_cipher(client->tls_ssl));
    return 0;
}

static int io_read_full(df_socket fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        if (stop_check || (io_client && io_client->cbs.on_tick)) {
            if (stop_check && stop_check()) {
                return -1;
            }
            int ready = df_socket_wait(fd);
            if (ready == 0 && io_client && io_client->cbs.on_tick &&
                io_client->cbs.on_tick(io_client->ud)) return -1;
            if (ready == 0 || (ready < 0 && df_socket_interrupted())) {
                continue;
            }
            if (ready < 0) {
                return -1;
            }
        }
        int r;
        if (io_client && io_client->fd == fd && io_client->tls_enabled) {
            r = SSL_read(io_client->tls_ssl, (char *)p + got, (int)(n - got));
            if (r <= 0) {
                int error = SSL_get_error(io_client->tls_ssl, r);
                if (error == SSL_ERROR_WANT_READ) {
                    if (tls_wait(fd, DF_POLL_READ) != 0) return -1;
                    continue;
                }
                if (error == SSL_ERROR_WANT_WRITE) {
                    if (tls_wait(fd, DF_POLL_WRITE) != 0) return -1;
                    continue;
                }
                if (error == SSL_ERROR_ZERO_RETURN) return 0;
                tls_log_error("read");
                return -1;
            }
        } else {
            r = recv(fd, (char *)p + got, (int)(n - got), 0);
        }
        if (r < 0) {
            if (df_socket_interrupted()) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return 0; /* EOF */
        }
        got += (size_t)r;
    }
    return 1;
}

static int io_write_full(df_socket fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < n) {
        int r;
        if (io_client && io_client->fd == fd && io_client->tls_enabled) {
            r = SSL_write(io_client->tls_ssl, (const char *)p + sent, (int)(n - sent));
            if (r <= 0) {
                int error = SSL_get_error(io_client->tls_ssl, r);
                if (error == SSL_ERROR_WANT_READ) {
                    if (tls_wait(fd, DF_POLL_READ) != 0) return -1;
                    continue;
                }
                if (error == SSL_ERROR_WANT_WRITE) {
                    if (tls_wait(fd, DF_POLL_WRITE) != 0) return -1;
                    continue;
                }
                tls_log_error("write");
                return -1;
            }
        } else {
#ifdef MSG_NOSIGNAL
            r = send(fd, (const char *)p + sent, (int)(n - sent), MSG_NOSIGNAL);
#else
            r = send(fd, (const char *)p + sent, (int)(n - sent), 0);
#endif
        }
        if (r < 0) {
            if (df_socket_interrupted()) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
#ifndef _WIN32
            errno = EPIPE;
#endif
            return -1;
        }
        sent += (size_t)r;
    }
    return 0;
}

/* ---- packet framing (deskflow PacketStreamFilter) ------------------------ */

/* Send one length-prefixed packet. */
static int send_packet(df_socket fd, const void *payload, uint32_t len)
{
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)(len & 0xff);
    if (io_write_full(fd, hdr, 4) != 0) {
        return -1;
    }
    return io_write_full(fd, payload, len);
}

/*
 * Read one length-prefixed packet into a freshly malloc'd buffer.
 * Returns the payload length, or -1 on error, or 0 on EOF.  *out is set to
 * the allocated buffer (caller frees) on success.
 */
static int recv_packet(df_socket fd, uint8_t **out)
{
    uint8_t hdr[4];
    int r = io_read_full(fd, hdr, 4);
    if (r <= 0) {
        return r; /* 0 = EOF, -1 = error */
    }
    uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                   ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
    if (len == 0 || len > DF_MAX_MESSAGE) {
        LOG_ERR("invalid packet length %u\n", len);
        return -1;
    }
    uint8_t *buf = malloc(len);
    if (!buf) {
        return -1;
    }
    if (io_read_full(fd, buf, len) != 1) {
        free(buf);
        return -1;
    }
    *out = buf;
    return (int)len;
}

/* ---- in-memory message parser ------------------------------------------- */

struct msg {
    const uint8_t *data;
    size_t len;
    size_t pos;
};

static int msg_read_u8(struct msg *m, uint8_t *out)
{
    if (m->pos + 1 > m->len) {
        return -1;
    }
    *out = m->data[m->pos++];
    return 0;
}

static int msg_read_u16(struct msg *m, uint16_t *out)
{
    if (m->pos + 2 > m->len) {
        return -1;
    }
    *out = (uint16_t)((m->data[m->pos] << 8) | m->data[m->pos + 1]);
    m->pos += 2;
    return 0;
}

static int msg_read_u32(struct msg *m, uint32_t *out)
{
    if (m->pos + 4 > m->len) {
        return -1;
    }
    *out = ((uint32_t)m->data[m->pos] << 24) |
           ((uint32_t)m->data[m->pos + 1] << 16) |
           ((uint32_t)m->data[m->pos + 2] << 8) |
           (uint32_t)m->data[m->pos + 3];
    m->pos += 4;
    return 0;
}

/* Skip a length-prefixed string (%s: 4-byte length + bytes). */
static int msg_skip_string(struct msg *m)
{
    uint32_t n;
    if (msg_read_u32(m, &n) != 0) {
        return -1;
    }
    if (m->pos + n > m->len) {
        return -1;
    }
    m->pos += n;
    return 0;
}

/* ---- message builders ---------------------------------------------------- */

static int send_hello_back(df_socket fd, const char *protocol, const char *name,
                           uint16_t minor)
{
    size_t name_len = strlen(name);
    uint8_t *buf = malloc(7 + 2 + 2 + 4 + name_len);
    if (!buf) {
        return -1;
    }
    size_t p = 0;
    memcpy(buf + p, protocol, 7); p += 7;
    buf[p++] = (uint8_t)(DF_PROTOCOL_MAJOR >> 8);
    buf[p++] = (uint8_t)(DF_PROTOCOL_MAJOR & 0xff);
    buf[p++] = (uint8_t)(minor >> 8);
    buf[p++] = (uint8_t)(minor & 0xff);
    buf[p++] = (uint8_t)(name_len >> 24);
    buf[p++] = (uint8_t)(name_len >> 16);
    buf[p++] = (uint8_t)(name_len >> 8);
    buf[p++] = (uint8_t)(name_len & 0xff);
    memcpy(buf + p, name, name_len); p += name_len;

    int r = send_packet(fd, buf, (uint32_t)p);
    free(buf);
    return r;
}

static int send_dinfo(df_socket fd, int32_t w, int32_t h)
{
    uint8_t buf[4 + 7 * 2];
    size_t p = 0;
    memcpy(buf, "DINF", 4); p += 4;
    /* x, y, w, h, warp(obsolete), mx, my */
    uint16_t vals[7] = { 0, 0, (uint16_t)w, (uint16_t)h, 0,
                         (uint16_t)(w / 2), (uint16_t)(h / 2) };
    for (int i = 0; i < 7; ++i) {
        buf[p++] = (uint8_t)(vals[i] >> 8);
        buf[p++] = (uint8_t)(vals[i] & 0xff);
    }
    return send_packet(fd, buf, (uint32_t)p);
}

static int send_noop(df_socket fd)
{
    return send_packet(fd, "CNOP", 4);
}

static int send_keepalive(df_socket fd)
{
    return send_packet(fd, "CALV", 4);
}

bool df_client_clipboard_enabled(const df_client *c)
{
    return c && c->proto_minor >= 6 && c->clipboard_requested && c->clipboard_server && c->clipboard_limit;
}

static void clipboard_clear(df_client *c)
{
    free(c->clipboard_local); c->clipboard_local = NULL;
    c->clipboard_local_size = 0; c->clipboard_owned = false;
    for (unsigned i = 0; i < 2; ++i) {
        free(c->clipboard_rx[i].data);
        memset(&c->clipboard_rx[i], 0, sizeof(c->clipboard_rx[i]));
    }
}

void df_client_enable_clipboard(df_client *c, bool enabled)
{
    if (!c) return;
    if (enabled && c->proto_minor < 6)
        fprintf(stderr, "WARN: clipboard requires Deskflow protocol 1.6 or newer\n");
    c->clipboard_requested = enabled;
    if (!enabled) clipboard_clear(c);
    if (c->cbs.on_clipboard_enabled)
        c->cbs.on_clipboard_enabled(c->ud, df_client_clipboard_enabled(c));
}

static int clipboard_send_chunk(df_client *c, uint8_t mark, const void *data, size_t size)
{
    uint8_t *packet = malloc(14 + size);
    if (!packet) return -1;
    memcpy(packet, "DCLP", 4); packet[4] = 0;
    otg_clipboard_put32(packet + 5, c->clipboard_seq);
    packet[9] = mark; otg_clipboard_put32(packet + 10, (uint32_t)size);
    if (size) memcpy(packet + 14, data, size);
    int result = send_packet(c->fd, packet, (uint32_t)(14 + size));
    free(packet);
    return result;
}

static int clipboard_send(df_client *c)
{
    if (!df_client_clipboard_enabled(c) || !c->clipboard_owned || !c->clipboard_local) return 0;
    if (c->clipboard_local_size > c->clipboard_limit) {
        free(c->clipboard_local); c->clipboard_local = NULL; c->clipboard_local_size = 0;
        return 0;
    }
    char size[32];
    int n = snprintf(size, sizeof(size), "%zu", c->clipboard_local_size);
    if (clipboard_send_chunk(c, 1, size, (size_t)n)) return -1;
    for (size_t pos = 0; pos < c->clipboard_local_size;) {
        size_t chunk = c->clipboard_local_size - pos;
        if (chunk > 32768) chunk = 32768;
        if (clipboard_send_chunk(c, 2, c->clipboard_local + pos, chunk)) return -1;
        pos += chunk;
    }
    if (clipboard_send_chunk(c, 3, NULL, 0)) return -1;
    /* Keep ownership, but do not resend unchanged data on the next leave. */
    free(c->clipboard_local); c->clipboard_local = NULL; c->clipboard_local_size = 0;
    return 0;
}

int df_client_set_clipboard(df_client *c, const char *text, size_t size)
{
    if (!df_client_clipboard_enabled(c)) return 0;
    if (size > OTG_CLIPBOARD_TEXT_MAX || size + 12 > c->clipboard_limit ||
        !otg_clipboard_valid_text(text, size)) {
        fprintf(stderr, "WARN: clipboard text not shared (invalid or exceeds size limit)\n");
        return 0;
    }
    size_t length;
    uint8_t *data = otg_clipboard_encode(text, size, &length);
    if (!data) return -1;
    free(c->clipboard_local); c->clipboard_local = data; c->clipboard_local_size = length;
    c->clipboard_owned = true;
    uint8_t grab[9] = {'C', 'C', 'L', 'P', 0};
    otg_clipboard_put32(grab + 5, c->clipboard_seq);
    if (send_packet(c->fd, grab, sizeof(grab))) return -1;
    return c->clipboard_active ? 0 : clipboard_send(c);
}

/* DCLP 1.6+: START contains decimal marshalled size, CHUNK raw bytes, END empty.
 * Bound allocation, validate identity/sequence, and drain oversize transfers. */
static int clipboard_receive(df_client *c, uint8_t id, uint32_t seq, uint8_t mark,
                             const uint8_t *data, size_t size)
{
    if (id >= 2 || !df_client_clipboard_enabled(c)) return 0;
    if (mark == 1) {
        free(c->clipboard_rx[id].data);
        memset(&c->clipboard_rx[id], 0, sizeof(c->clipboard_rx[id]));
        size_t expected = 0;
        if (!size || size > 20) goto invalid;
        for (size_t i = 0; i < size; ++i) {
            if (data[i] < '0' || data[i] > '9' || expected > (SIZE_MAX - 9) / 10) goto invalid;
            expected = expected * 10 + (data[i] - '0');
        }
        c->clipboard_rx[id].expected = expected;
        if (!id) {
            free(c->clipboard_local); c->clipboard_local = NULL;
            c->clipboard_local_size = 0; c->clipboard_owned = false;
        }
        c->clipboard_rx[id].sequence = seq;
        c->clipboard_rx[id].active = true;
        c->clipboard_rx[id].drop = id != 0 || expected > c->clipboard_limit ||
                                  expected > OTG_CLIPBOARD_WIRE_MAX;
        if (c->clipboard_rx[id].drop) {
            if (!id) fprintf(stderr, "WARN: incoming clipboard exceeds size limit; not shared\n");
        } else {
            c->clipboard_rx[id].data = malloc(expected ? expected : 1);
            if (!c->clipboard_rx[id].data) goto invalid;
        }
        return 0;
    }
    if (!c->clipboard_rx[id].active || c->clipboard_rx[id].sequence != seq) goto invalid;
    if (mark == 2) {
        if (size > c->clipboard_rx[id].expected - c->clipboard_rx[id].size) goto invalid;
        if (!c->clipboard_rx[id].drop && size)
            memcpy(c->clipboard_rx[id].data + c->clipboard_rx[id].size, data, size);
        c->clipboard_rx[id].size += size;
        return 0;
    }
    if (mark != 3 || size || c->clipboard_rx[id].size != c->clipboard_rx[id].expected) goto invalid;
    if (!c->clipboard_rx[id].drop) {
        const char *text = NULL;
        size_t length = 0;
        int decoded = otg_clipboard_decode(c->clipboard_rx[id].data,
                                          c->clipboard_rx[id].size, &text, &length);
        if (decoded < 0) fprintf(stderr, "WARN: unsupported or invalid clipboard text; not shared\n");
        if (!id && decoded == 1) {
            free(c->clipboard_local); c->clipboard_local = NULL;
            c->clipboard_local_size = 0; c->clipboard_owned = false;
            if (c->cbs.on_clipboard) c->cbs.on_clipboard(c->ud, text, length);
        }
    }
    free(c->clipboard_rx[id].data);
    memset(&c->clipboard_rx[id], 0, sizeof(c->clipboard_rx[id]));
    return 0;
invalid:
    free(c->clipboard_rx[id].data);
    memset(&c->clipboard_rx[id], 0, sizeof(c->clipboard_rx[id]));
    fprintf(stderr, "WARN: invalid clipboard transfer ignored\n");
    return 0; /* Clipboard errors must not disconnect keyboard/mouse. */
}

/* ---- connect + handshake ------------------------------------------------ */

/* GUI cancellation must also work while a TCP connection is pending. DNS is
 * still resolved by the OS; the GUI applies an outer startup deadline. */
static int socket_connect(df_socket fd, const struct sockaddr *addr, int len)
{
    if (!stop_check) {
        return connect(fd, addr, len);
    }
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) {
        return -1;
    }
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
#endif
    int result = connect(fd, addr, len);
    if (result != 0) {
        for (int attempt = 0; attempt < 20 && !stop_check(); ++attempt) {
            fd_set writefds, errors;
            FD_ZERO(&writefds);
            FD_ZERO(&errors);
            FD_SET(fd, &writefds);
            FD_SET(fd, &errors);
            struct timeval timeout = {0, 500000};
#ifdef _WIN32
            int ready = select(0, NULL, &writefds, &errors, &timeout);
#else
            int ready = select(fd + 1, NULL, &writefds, &errors, &timeout);
#endif
            if (ready < 0 && df_socket_interrupted()) {
                continue;
            }
            if (ready < 0) {
                break;
            }
            if (ready > 0) {
                int error = 0;
#ifdef _WIN32
                int size = sizeof(error);
#else
                socklen_t size = sizeof(error);
#endif
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &size) == 0 && !error) {
                    result = 0;
                }
                break;
            }
        }
    }
#ifdef _WIN32
    nonblocking = 0;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) {
        return -1;
    }
    DWORD send_timeout = 1000;
#else
    if (fcntl(fd, F_SETFL, flags) < 0) {
        return -1;
    }
    struct timeval send_timeout = {1, 0};
#endif
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                     (const char *)&send_timeout, sizeof(send_timeout));
    return stop_check() ? -1 : result;
}

static df_socket tcp_connect(const char *host, uint16_t port)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int g = getaddrinfo(host, portstr, &hints, &res);
    if (g != 0) {
        LOG_ERR("getaddrinfo(%s) failed: %d\n", host, g);
        return DF_INVALID_SOCKET;
    }

    df_socket fd = DF_INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (stop_check && stop_check()) {
            break;
        }
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == DF_INVALID_SOCKET) {
            continue;
        }
        if (socket_suppress_sigpipe(fd) != 0) {
            LOG_ERR("could not disable socket SIGPIPE: %d\n", df_socket_error());
            df_close_socket(fd);
            fd = DF_INVALID_SOCKET;
            continue;
        }
        if (socket_connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
            socket_enable_low_latency(fd);
            break;
        }
        df_close_socket(fd);
        fd = DF_INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (fd == DF_INVALID_SOCKET) {
        LOG_ERR("could not connect to %s:%u\n", host, (unsigned)port);
        return DF_INVALID_SOCKET;
    }
    return fd;
}

df_client *df_client_connect_ex(const char *host, uint16_t port, const char *name,
                                int32_t screen_w, int32_t screen_h,
                                const df_client_callbacks *cbs, void *ud,
                                const df_client_tls_options *tls)
{
    if (df_socket_init() != 0) {
        return NULL;
    }
    df_socket fd = tcp_connect(host, port);
    if (fd == DF_INVALID_SOCKET) {
        df_socket_cleanup();
        return NULL;
    }

    df_client *c = calloc(1, sizeof(*c));
    if (!c) {
        df_close_socket(fd);
        df_socket_cleanup();
        return NULL;
    }
    c->fd = fd;
    c->screen_w = screen_w;
    c->screen_h = screen_h;
    c->cbs = *cbs;
    c->ud = ud;
    c->clipboard_server = true;
    c->clipboard_limit = OTG_CLIPBOARD_WIRE_MAX;

    if (tls_setup(c, tls) != 0 || tls_handshake(c) != 0) {
        goto fail;
    }
    /* Packet helpers use the active client to select TLS or plain socket I/O.
     * Keep it active through the initial hello and optional clipboard setup. */
    io_client = c;
    io_wait_interval_ms = c->cbs.tick_interval_ms > 0 ? c->cbs.tick_interval_ms : 500;

    /* 1. server -> hello packet: "%7s%2i%2i" (protocol name + major + minor). */
    uint8_t *hello = NULL;
    int hello_len = recv_packet(fd, &hello);
    if (hello_len < 11) {
        LOG_ERR("failed to read server hello\n");
        free(hello);
        goto fail;
    }

    char protocol[8];
    memcpy(protocol, hello, 7);
    protocol[7] = '\0';
    uint16_t server_major = (uint16_t)((hello[7] << 8) | hello[8]);
    uint16_t server_minor = (uint16_t)((hello[9] << 8) | hello[10]);
    free(hello);

    if (strncmp(protocol, "Synergy", 7) != 0 &&
        strncmp(protocol, "Barrier", 7) != 0) {
        LOG_ERR("unknown protocol '%s'\n", protocol);
        goto fail;
    }
    if (server_major != DF_PROTOCOL_MAJOR) {
        LOG_ERR("incompatible protocol %u.%u\n", server_major, server_minor);
        goto fail;
    }
    LOG_INFO("server protocol %s %u.%u\n", protocol, server_major, server_minor);

    /* Negotiate the minor version (deskflow's downgrade rule). */
    uint16_t minor = server_minor < DF_PROTOCOL_MINOR ? server_minor
                                                      : DF_PROTOCOL_MINOR;

    /* 2. client -> hello back packet (echo the server's protocol name). */
    if (send_hello_back(fd, protocol, name, minor) != 0) {
        LOG_ERR("failed to send hello back\n");
        goto fail;
    }
    c->proto_minor = minor;

    return c;

fail:
    if (io_client == c) io_client = NULL;
    io_wait_interval_ms = 500;
    if (c->tls_ssl) SSL_free(c->tls_ssl);
    if (c->tls_ctx) SSL_CTX_free(c->tls_ctx);
    free(c->tls_fingerprint);
    free(c->tls_fingerprint_file);
    df_close_socket(fd);
    df_socket_cleanup();
    free(c);
    return NULL;
}

df_client *df_client_connect(const char *host, uint16_t port, const char *name,
                             int32_t screen_w, int32_t screen_h,
                             const df_client_callbacks *cbs, void *ud)
{
    return df_client_connect_ex(host, port, name, screen_w, screen_h, cbs, ud, NULL);
}

/* ---- message dispatch ---------------------------------------------------- */

int df_client_resize(df_client *c, int32_t width, int32_t height)
{
    if (!c || width < 1 || width > INT16_MAX || height < 1 || height > INT16_MAX) return -1;
    if (!c->accepted || c->info_pending) return 1;
    if (width == c->screen_w && height == c->screen_h) return 0;
    if (send_dinfo(c->fd, width, height)) return -1;
    c->screen_w = width; c->screen_h = height;
    ++c->info_pending; c->resizing = true;
    return 0;
}

static int handle_message(df_client *c, const char code[4], struct msg *m)
{
    uint16_t a, b, ctr;

    if (memcmp(code, "QINF", 4) == 0) {
        ++c->info_pending;
        return send_dinfo(c->fd, c->screen_w, c->screen_h);
    }
    if (memcmp(code, "CIAK", 4) == 0) {
        if (c->info_pending) --c->info_pending;
        if (!c->info_pending) c->resizing = false;
        return 0;
    }
    if (memcmp(code, "DSOP", 4) == 0) {
        /* set options: "%4I" = 4-byte count + count*4 bytes */
        uint32_t n;
        if (msg_read_u32(m, &n) != 0) {
            return -1;
        }
        if (n % 2 || n > (m->len - m->pos) / 4) return -1;
        bool was_enabled = df_client_clipboard_enabled(c);
        for (uint32_t i = 0; i < n; i += 2) {
            uint32_t opt, value;
            if (msg_read_u32(m, &opt) != 0 || msg_read_u32(m, &value) != 0) {
                return -1;
            }
            if (opt == UINT32_C(0x434c5053)) c->clipboard_server = value != 0; /* CLPS */
            if (opt == UINT32_C(0x434c535a)) /* CLSZ, KiB, negative means disabled */
                c->clipboard_limit = (int32_t)value <= 0 ? 0 :
                    value > OTG_CLIPBOARD_WIRE_MAX / 1024 ? OTG_CLIPBOARD_WIRE_MAX : (size_t)value * 1024;
        }
        if (was_enabled != df_client_clipboard_enabled(c)) {
            if (!df_client_clipboard_enabled(c)) clipboard_clear(c);
            if (c->cbs.on_clipboard_enabled)
                c->cbs.on_clipboard_enabled(c->ud, df_client_clipboard_enabled(c));
        }
        if (!c->accepted) {
            c->accepted = 1;
            if (c->cbs.on_connected) {
                c->cbs.on_connected(c->ud);
            }
        }
        return 0;
    }
    if (memcmp(code, "CROP", 4) == 0) {
        c->clipboard_server = true; c->clipboard_limit = OTG_CLIPBOARD_WIRE_MAX;
        clipboard_clear(c);
        if (c->cbs.on_clipboard_enabled)
            c->cbs.on_clipboard_enabled(c->ud, df_client_clipboard_enabled(c));
        return 0; /* reset options */
    }
    if (memcmp(code, "CALV", 4) == 0) {
        return send_keepalive(c->fd); /* echo keep-alive */
    }
    if (memcmp(code, "CNOP", 4) == 0) {
        return 0;
    }
    if (memcmp(code, "CBYE", 4) == 0) {
        return 1; /* server asked us to close */
    }
    if (memcmp(code, "CINN", 4) == 0) {
        uint16_t x, y, mask;
        uint32_t seq;
        if (msg_read_u16(m, &x) != 0 || msg_read_u16(m, &y) != 0 ||
            msg_read_u32(m, &seq) != 0 || msg_read_u16(m, &mask) != 0) {
            return -1;
        }
        c->clipboard_active = true; c->clipboard_seq = seq;
        if (c->cbs.on_enter) {
            c->cbs.on_enter(c->ud, (int16_t)x, (int16_t)y, seq, mask);
        }
        return 0;
    }
    if (memcmp(code, "COUT", 4) == 0) {
        c->clipboard_active = false;
        if (c->cbs.on_leave) {
            c->cbs.on_leave(c->ud);
        }
        return clipboard_send(c);
    }
    if (memcmp(code, "DKDN", 4) == 0) {
        /* key down 1.1-1.7: "%2i%2i%2i" */
        if (msg_read_u16(m, &a) != 0 || msg_read_u16(m, &b) != 0 ||
            msg_read_u16(m, &ctr) != 0) {
            return -1;
        }
        if (c->cbs.on_key_down) {
            c->cbs.on_key_down(c->ud, a, b);
        }
        return 0;
    }
    if (memcmp(code, "DKDL", 4) == 0) {
        /* key down 1.8: "%2i%2i%2i%s" */
        if (msg_read_u16(m, &a) != 0 || msg_read_u16(m, &b) != 0 ||
            msg_read_u16(m, &ctr) != 0 || msg_skip_string(m) != 0) {
            return -1;
        }
        if (c->cbs.on_key_down) {
            c->cbs.on_key_down(c->ud, a, b);
        }
        return 0;
    }
    if (memcmp(code, "DKUP", 4) == 0) {
        if (msg_read_u16(m, &a) != 0 || msg_read_u16(m, &b) != 0 ||
            msg_read_u16(m, &ctr) != 0) {
            return -1;
        }
        if (c->cbs.on_key_up) {
            c->cbs.on_key_up(c->ud, a, b);
        }
        return 0;
    }
    if (memcmp(code, "DKRP", 4) == 0) {
        /* key repeat: "%2i%2i%2i%2i" (1.1-1.7) or "%2i%2i%2i%2i%s" (1.8). */
        uint16_t count;
        if (msg_read_u16(m, &a) != 0 || msg_read_u16(m, &b) != 0 ||
            msg_read_u16(m, &count) != 0 || msg_read_u16(m, &ctr) != 0) {
            return -1;
        }
        if (c->proto_minor >= 8 && msg_skip_string(m) != 0) {
            return -1;
        }
        if (c->cbs.on_key_repeat) {
            c->cbs.on_key_repeat(c->ud, a, b, (int32_t)count);
        }
        return 0;
    }
    if (memcmp(code, "DMDN", 4) == 0) {
        uint8_t btn;
        if (msg_read_u8(m, &btn) != 0) {
            return -1;
        }
        if (!c->resizing && c->cbs.on_mouse_down) {
            c->cbs.on_mouse_down(c->ud, btn);
        }
        return 0;
    }
    if (memcmp(code, "DMUP", 4) == 0) {
        uint8_t btn;
        if (msg_read_u8(m, &btn) != 0) {
            return -1;
        }
        if (c->cbs.on_mouse_up) {
            c->cbs.on_mouse_up(c->ud, btn);
        }
        return 0;
    }
    if (memcmp(code, "DMMV", 4) == 0) {
        if (msg_read_u16(m, &a) != 0 || msg_read_u16(m, &b) != 0) {
            return -1;
        }
        if (!c->resizing && c->cbs.on_mouse_move) {
            c->cbs.on_mouse_move(c->ud, (int16_t)a, (int16_t)b);
        }
        return 0;
    }
    if (memcmp(code, "DMRM", 4) == 0) {
        if (msg_read_u16(m, &a) != 0 || msg_read_u16(m, &b) != 0) {
            return -1;
        }
        if (!c->resizing && c->cbs.on_mouse_rel_move) {
            c->cbs.on_mouse_rel_move(c->ud, (int16_t)a, (int16_t)b);
        }
        return 0;
    }
    if (memcmp(code, "DMWM", 4) == 0) {
        if (msg_read_u16(m, &a) != 0 || msg_read_u16(m, &b) != 0) {
            return -1;
        }
        if (!c->resizing && c->cbs.on_mouse_wheel) {
            c->cbs.on_mouse_wheel(c->ud, (int16_t)a, (int16_t)b);
        }
        return 0;
    }
    if (memcmp(code, "CCLP", 4) == 0) {
        /* grab clipboard: "%1i%4i" */
        uint8_t id;
        uint32_t seq;
        if (msg_read_u8(m, &id) != 0 || msg_read_u32(m, &seq) != 0) {
            return -1;
        }
        if (!id && df_client_clipboard_enabled(c)) {
            clipboard_clear(c);
        }
        return 0;
    }
    if (memcmp(code, "DCLP", 4) == 0) {
        /* set clipboard: "%1i%4i%1i%s" */
        uint8_t id, marker;
        uint32_t seq, size;
        if (msg_read_u8(m, &id) != 0 || msg_read_u32(m, &seq) != 0 ||
            msg_read_u8(m, &marker) != 0 || msg_read_u32(m, &size) != 0 || size > m->len - m->pos) {
            return -1;
        }
        return clipboard_receive(c, id, seq, marker, m->data + m->pos, size);
    }
    if (memcmp(code, "CSEC", 4) == 0) {
        uint8_t on;
        if (msg_read_u8(m, &on) != 0) {
            return -1;
        }
        return 0; /* screensaver, ignore */
    }
    if (memcmp(code, "EICV", 4) == 0) {
        uint16_t maj, min;
        if (msg_read_u16(m, &maj) != 0 || msg_read_u16(m, &min) != 0) {
            return -1;
        }
        LOG_ERR("server reports incompatible version %u.%u\n", maj, min);
        return -1;
    }
    if (memcmp(code, "EBSY", 4) == 0) {
        LOG_ERR("server already has a client with our name\n");
        return -1;
    }
    if (memcmp(code, "EUNK", 4) == 0) {
        LOG_ERR("server refused our screen name\n");
        return -1;
    }
    if (memcmp(code, "EBAD", 4) == 0) {
        LOG_ERR("server reported a protocol error\n");
        return -1;
    }

    /* Ignore anything else (language sync "LSYN", secure-input "SECN", file
     * transfer "DFTR", drag info "DDRG", future extensions, ...).  The whole
     * packet has already been read, so the stream stays in sync; we simply
     * acknowledge with a no-op like the real deskflow client does. */
    LOG_DBG("ignoring message %.4s\n", code);
    return 0;
}

int df_client_run(df_client *c, volatile sig_atomic_t *stop)
{
    df_socket fd = c->fd;
    int result = 0;
    /* A partial TCP packet must not stall an asynchronous transport's pending
     * replies/reports for the legacy 500ms cancellation interval. */
    io_wait_interval_ms = c->cbs.tick_interval_ms > 0
                             ? c->cbs.tick_interval_ms : 500;
    io_client = c;

    for (;;) {
        if ((stop && *stop) || (stop_check && stop_check())) {
            break;
        }

        /* Service asynchronous transports even under continuous TCP input.
         * The legacy idle hook remains timeout-only for USB callers. */
        if (c->cbs.on_tick && c->cbs.on_tick(c->ud)) {
            break;
        }
        int timeout_ms = c->cbs.tick_interval_ms > 0
                             ? c->cbs.tick_interval_ms : 500;
        int pr = df_socket_wait_timeout(fd, timeout_ms);
        if (pr < 0) {
            if (df_socket_interrupted()) {
                continue;
            }
            LOG_ERR("socket wait failed: %d\n", df_socket_error());
            result = -1;
            break;
        }
        if (pr == 0) {
            if (c->cbs.on_idle && c->cbs.on_idle(c->ud)) {
                result = 0;
                break;
            }
            continue; /* timeout, loop to check stop flag */
        }

        uint8_t *pkt = NULL;
        int plen = recv_packet(fd, &pkt);
        if (plen == 0) {
            LOG_INFO("server closed the connection\n");
            result = 0;
            break;
        }
        if (plen < 0) {
            if ((stop && *stop) || (stop_check && stop_check())) {
                LOG_INFO("socket read cancelled by stop check\n");
            } else {
                LOG_ERR("socket read failed: %d\n", df_socket_error());
            }
            result = -1;
            break;
        }

        if (plen < 4) {
            free(pkt);
            result = -1;
            break;
        }
        char code[4];
        memcpy(code, pkt, 4);
        struct msg m = { .data = pkt, .len = (size_t)plen, .pos = 4 };

        int h = handle_message(c, code, &m);
        free(pkt);

        if (h < 0) {
            result = -1;
            break;
        }
        if (h > 0) {
            result = 0; /* server asked us to close (CBYE) */
            break;
        }

        /* Acknowledge every handled message with a no-op, matching the
         * deskflow client's handleMessage() behaviour. */
        if (send_noop(fd) != 0) {
            result = -1;
            break;
        }
    }

    io_wait_interval_ms = 500;
    io_client = NULL;
    if (c->cbs.on_disconnected) {
        c->cbs.on_disconnected(c->ud);
    }
    return result;
}

void df_client_disconnect(df_client *c)
{
    if (!c) {
        return;
    }
    if (io_client == c) io_client = NULL;
    if (c->tls_ssl) {
        SSL_shutdown(c->tls_ssl);
        SSL_free(c->tls_ssl);
        c->tls_ssl = NULL;
    }
    if (c->tls_ctx) {
        SSL_CTX_free(c->tls_ctx);
        c->tls_ctx = NULL;
    }
    free(c->tls_fingerprint);
    free(c->tls_fingerprint_file);
    c->tls_fingerprint = NULL;
    c->tls_fingerprint_file = NULL;
    if (c->fd != DF_INVALID_SOCKET) {
        df_close_socket(c->fd);
        c->fd = DF_INVALID_SOCKET;
    }
    df_socket_cleanup();
    clipboard_clear(c);
    free(c);
}
