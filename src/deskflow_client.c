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

#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "deskflow_client.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DF_PROTOCOL_MAJOR 1
#define DF_PROTOCOL_MINOR 8
#define DF_MAX_MESSAGE    (4 * 1024 * 1024) /* PROTOCOL_MAX_MESSAGE_LENGTH */

#define LOG_ERR(...)  fprintf(stderr, "ERROR: " __VA_ARGS__)
#define LOG_INFO(...) fprintf(stderr, "INFO:  " __VA_ARGS__)
#define LOG_DBG(...)  fprintf(stderr, "DEBUG: " __VA_ARGS__)

struct df_client {
    int fd;
    int32_t screen_w;
    int32_t screen_h;
    uint16_t proto_minor;   /* negotiated protocol minor version */
    df_client_callbacks cbs;
    void *ud;
};

/* ---- low-level socket I/O ----------------------------------------------- */

static int socket_suppress_sigpipe(int fd)
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

static int io_read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) {
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

static int io_write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < n) {
#ifdef MSG_NOSIGNAL
        ssize_t r = send(fd, p + sent, n - sent, MSG_NOSIGNAL);
#else
        ssize_t r = send(fd, p + sent, n - sent, 0);
#endif
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            errno = EPIPE;
            return -1;
        }
        sent += (size_t)r;
    }
    return 0;
}

/* ---- packet framing (deskflow PacketStreamFilter) ------------------------ */

/* Send one length-prefixed packet. */
static int send_packet(int fd, const void *payload, uint32_t len)
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
static int recv_packet(int fd, uint8_t **out)
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

static int send_hello_back(int fd, const char *protocol, const char *name,
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

static int send_dinfo(int fd, int32_t w, int32_t h)
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

static int send_noop(int fd)
{
    return send_packet(fd, "CNOP", 4);
}

static int send_keepalive(int fd)
{
    return send_packet(fd, "CALV", 4);
}

/* ---- connect + handshake ------------------------------------------------ */

static int tcp_connect(const char *host, uint16_t port)
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
        LOG_ERR("getaddrinfo(%s): %s\n", host, gai_strerror(g));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (socket_suppress_sigpipe(fd) != 0) {
            LOG_ERR("could not disable socket SIGPIPE: %s\n", strerror(errno));
            close(fd);
            fd = -1;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        LOG_ERR("could not connect to %s:%u\n", host, (unsigned)port);
        return -1;
    }
    return fd;
}

df_client *df_client_connect(const char *host, uint16_t port, const char *name,
                             int32_t screen_w, int32_t screen_h,
                             const df_client_callbacks *cbs, void *ud)
{
    int fd = tcp_connect(host, port);
    if (fd < 0) {
        return NULL;
    }

    df_client *c = calloc(1, sizeof(*c));
    if (!c) {
        close(fd);
        return NULL;
    }
    c->fd = fd;
    c->screen_w = screen_w;
    c->screen_h = screen_h;
    c->cbs = *cbs;
    c->ud = ud;

    /* 1. server -> hello packet: "%7s%2i%2i" (protocol name + major + minor). */
    uint8_t *hello = NULL;
    int hello_len = recv_packet(fd, &hello);
    if (hello_len < 11) {
        LOG_ERR("failed to read server hello\n");
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
    close(fd);
    free(c);
    return NULL;
}

/* ---- message dispatch ---------------------------------------------------- */

static int handle_message(df_client *c, const char code[4], struct msg *m)
{
    uint16_t a, b, ctr;

    if (memcmp(code, "QINF", 4) == 0) {
        return send_dinfo(c->fd, c->screen_w, c->screen_h);
    }
    if (memcmp(code, "CIAK", 4) == 0) {
        return 0;
    }
    if (memcmp(code, "DSOP", 4) == 0) {
        /* set options: "%4I" = 4-byte count + count*4 bytes */
        uint32_t n;
        if (msg_read_u32(m, &n) != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t opt;
            if (msg_read_u32(m, &opt) != 0) {
                return -1;
            }
        }
        return 0;
    }
    if (memcmp(code, "CROP", 4) == 0) {
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
        if (c->cbs.on_enter) {
            c->cbs.on_enter(c->ud, (int16_t)x, (int16_t)y, seq, mask);
        }
        return 0;
    }
    if (memcmp(code, "COUT", 4) == 0) {
        if (c->cbs.on_leave) {
            c->cbs.on_leave(c->ud);
        }
        return 0;
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
        if (c->cbs.on_mouse_down) {
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
        if (c->cbs.on_mouse_move) {
            c->cbs.on_mouse_move(c->ud, (int16_t)a, (int16_t)b);
        }
        return 0;
    }
    if (memcmp(code, "DMRM", 4) == 0) {
        if (msg_read_u16(m, &a) != 0 || msg_read_u16(m, &b) != 0) {
            return -1;
        }
        if (c->cbs.on_mouse_rel_move) {
            c->cbs.on_mouse_rel_move(c->ud, (int16_t)a, (int16_t)b);
        }
        return 0;
    }
    if (memcmp(code, "DMWM", 4) == 0) {
        if (msg_read_u16(m, &a) != 0 || msg_read_u16(m, &b) != 0) {
            return -1;
        }
        if (c->cbs.on_mouse_wheel) {
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
        return 0;
    }
    if (memcmp(code, "DCLP", 4) == 0) {
        /* set clipboard: "%1i%4i%1i%s" */
        uint8_t id, marker;
        uint32_t seq;
        if (msg_read_u8(m, &id) != 0 || msg_read_u32(m, &seq) != 0 ||
            msg_read_u8(m, &marker) != 0 || msg_skip_string(m) != 0) {
            return -1;
        }
        return 0;
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
    int fd = c->fd;
    int result = 0;

    for (;;) {
        if (stop && *stop) {
            break;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERR("poll error: %s\n", strerror(errno));
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
            LOG_ERR("read error: %s\n", strerror(errno));
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
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    free(c);
}
