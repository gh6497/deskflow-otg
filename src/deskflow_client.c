/*
 * deskflow_client.c - see deskflow_client.h.
 *
 * Wire format notes (from deskflow ProtocolUtil.cpp):
 *   - integers are big-endian, 1/2/4 bytes (%i / %2i / %4i)
 *   - strings are length-prefixed: 4-byte big-endian length + bytes (%s);
 *     %1s / %2s use 1 / 2-byte length prefixes.
 *   - every message begins with a 4-byte code.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE 1

#include "deskflow_client.h"

#include <arpa/inet.h>
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

/* ---- low-level I/O ------------------------------------------------------ */

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
        ssize_t r = write(fd, p + sent, n - sent);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)r;
    }
    return 0;
}

static int read_u16(int fd, uint16_t *out)
{
    uint8_t b[2];
    if (io_read_full(fd, b, 2) != 1) {
        return -1;
    }
    *out = (uint16_t)((b[0] << 8) | b[1]);
    return 0;
}

static int read_u32(int fd, uint32_t *out)
{
    uint8_t b[4];
    if (io_read_full(fd, b, 4) != 1) {
        return -1;
    }
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    return 0;
}

/* Append a big-endian 2-byte / 4-byte integer to a growing buffer. */
static void put_u16(uint8_t **p, uint16_t v)
{
    (*p)[0] = (uint8_t)(v >> 8);
    (*p)[1] = (uint8_t)(v & 0xff);
    *p += 2;
}

static void put_u32(uint8_t **p, uint32_t v)
{
    (*p)[0] = (uint8_t)(v >> 24);
    (*p)[1] = (uint8_t)(v >> 16);
    (*p)[2] = (uint8_t)(v >> 8);
    (*p)[3] = (uint8_t)(v & 0xff);
    *p += 4;
}

/*
 * Read a length-prefixed string and discard it (the language code and
 * clipboard payload are not needed by this bridge).
 */
static int read_and_discard_string(int fd)
{
    uint32_t len;
    if (read_u32(fd, &len) != 0) {
        return -1;
    }
    if (len > 1024 * 1024) {
        return -1;
    }
    char buf[512];
    uint32_t left = len;
    while (left > 0) {
        uint32_t chunk = left < sizeof(buf) ? left : (uint32_t)sizeof(buf);
        if (io_read_full(fd, buf, chunk) != 1) {
            return -1;
        }
        left -= chunk;
    }
    return 0;
}

/* ---- message senders ---------------------------------------------------- */

static int send_hello_back(int fd, const char *protocol, const char *name,
                           uint16_t minor)
{
    /* kMsgHelloBack = "%7s%2i%2i%s" */
    size_t name_len = strlen(name);
    uint8_t buf[7 + 2 + 2 + 4 + name_len];
    uint8_t *p = buf;

    memcpy(p, protocol, 7);
    p += 7;
    put_u16(&p, DF_PROTOCOL_MAJOR);
    put_u16(&p, minor);
    put_u32(&p, (uint32_t)name_len);
    memcpy(p, name, name_len);
    p += name_len;

    return io_write_full(fd, buf, (size_t)(p - buf));
}

static int send_dinfo(int fd, int32_t w, int32_t h)
{
    /* kMsgDInfo = "DINF%2i%2i%2i%2i%2i%2i%2i"
     * x=0, y=0, w, h, warp=0, mx=w/2, my=h/2 */
    uint8_t buf[4 + 7 * 2];
    uint8_t *p = buf;
    memcpy(p, "DINF", 4);
    p += 4;
    put_u16(&p, 0);
    put_u16(&p, 0);
    put_u16(&p, (uint16_t)w);
    put_u16(&p, (uint16_t)h);
    put_u16(&p, 0);
    put_u16(&p, (uint16_t)(w / 2));
    put_u16(&p, (uint16_t)(h / 2));
    return io_write_full(fd, buf, sizeof(buf));
}

static int send_noop(int fd)
{
    return io_write_full(fd, "CNOP", 4);
}

static int send_keepalive(int fd)
{
    return io_write_full(fd, "CALV", 4);
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

    /* 1. server -> hello: "%7s%2i%2i" */
    char protocol[8];
    uint16_t server_major = 0, server_minor = 0;
    if (io_read_full(fd, protocol, 7) != 1 ||
        read_u16(fd, &server_major) != 0 ||
        read_u16(fd, &server_minor) != 0) {
        LOG_ERR("failed to read server hello\n");
        goto fail;
    }
    protocol[7] = '\0';

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

    /* Negotiate the minor version (must match the deskflow client's
     * downgrade rule). */
    uint16_t minor = server_minor < DF_PROTOCOL_MINOR ? server_minor
                                                      : DF_PROTOCOL_MINOR;

    /* 2. client -> hello back (echo the server's protocol name) */
    if (send_hello_back(fd, protocol, name, minor) != 0) {
        LOG_ERR("failed to send hello back\n");
        goto fail;
    }
    c->proto_minor = minor;

    /* 3. server -> QInfo, client -> DInfo happens in the message loop. */
    return c;

fail:
    close(fd);
    free(c);
    return NULL;
}

/* ---- message loop ------------------------------------------------------- */

static int handle_message(df_client *c, const char code[4])
{
    int fd = c->fd;
    uint16_t a, b, ctr;

    if (memcmp(code, "QINF", 4) == 0) {
        return send_dinfo(fd, c->screen_w, c->screen_h);
    }
    if (memcmp(code, "CIAK", 4) == 0) {
        return 0;
    }
    if (memcmp(code, "DSOP", 4) == 0) {
        /* set options: "%4I" = 4-byte count + count*4 bytes */
        uint32_t n;
        if (read_u32(fd, &n) != 0) {
            return -1;
        }
        char skip[256];
        for (uint32_t i = 0; i < n; ++i) {
            if (io_read_full(fd, skip, 4) != 1) {
                return -1;
            }
        }
        return 0;
    }
    if (memcmp(code, "CROP", 4) == 0) {
        return 0; /* reset options */
    }
    if (memcmp(code, "CALV", 4) == 0) {
        return send_keepalive(fd); /* echo keep-alive */
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
        if (read_u16(fd, &x) != 0 || read_u16(fd, &y) != 0 ||
            read_u32(fd, &seq) != 0 || read_u16(fd, &mask) != 0) {
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
        if (read_u16(fd, &a) != 0 || read_u16(fd, &b) != 0 ||
            read_u16(fd, &ctr) != 0) {
            return -1;
        }
        if (c->cbs.on_key_down) {
            c->cbs.on_key_down(c->ud, a, b);
        }
        return 0;
    }
    if (memcmp(code, "DKDL", 4) == 0) {
        /* key down 1.8: "%2i%2i%2i%s" */
        if (read_u16(fd, &a) != 0 || read_u16(fd, &b) != 0 ||
            read_u16(fd, &ctr) != 0 || read_and_discard_string(fd) != 0) {
            return -1;
        }
        if (c->cbs.on_key_down) {
            c->cbs.on_key_down(c->ud, a, b);
        }
        return 0;
    }
    if (memcmp(code, "DKUP", 4) == 0) {
        if (read_u16(fd, &a) != 0 || read_u16(fd, &b) != 0 ||
            read_u16(fd, &ctr) != 0) {
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
        if (read_u16(fd, &a) != 0 || read_u16(fd, &b) != 0 ||
            read_u16(fd, &count) != 0 || read_u16(fd, &ctr) != 0) {
            return -1;
        }
        if (c->proto_minor >= 8 && read_and_discard_string(fd) != 0) {
            return -1;
        }
        if (c->cbs.on_key_repeat) {
            c->cbs.on_key_repeat(c->ud, a, b, (int32_t)count);
        }
        return 0;
    }
    if (memcmp(code, "DMDN", 4) == 0) {
        uint8_t btn;
        if (io_read_full(fd, &btn, 1) != 1) {
            return -1;
        }
        if (c->cbs.on_mouse_down) {
            c->cbs.on_mouse_down(c->ud, btn);
        }
        return 0;
    }
    if (memcmp(code, "DMUP", 4) == 0) {
        uint8_t btn;
        if (io_read_full(fd, &btn, 1) != 1) {
            return -1;
        }
        if (c->cbs.on_mouse_up) {
            c->cbs.on_mouse_up(c->ud, btn);
        }
        return 0;
    }
    if (memcmp(code, "DMMV", 4) == 0) {
        if (read_u16(fd, &a) != 0 || read_u16(fd, &b) != 0) {
            return -1;
        }
        if (c->cbs.on_mouse_move) {
            c->cbs.on_mouse_move(c->ud, (int16_t)a, (int16_t)b);
        }
        return 0;
    }
    if (memcmp(code, "DMRM", 4) == 0) {
        if (read_u16(fd, &a) != 0 || read_u16(fd, &b) != 0) {
            return -1;
        }
        if (c->cbs.on_mouse_rel_move) {
            c->cbs.on_mouse_rel_move(c->ud, (int16_t)a, (int16_t)b);
        }
        return 0;
    }
    if (memcmp(code, "DMWM", 4) == 0) {
        if (read_u16(fd, &a) != 0 || read_u16(fd, &b) != 0) {
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
        if (io_read_full(fd, &id, 1) != 1 || read_u32(fd, &seq) != 0) {
            return -1;
        }
        return 0;
    }
    if (memcmp(code, "DCLP", 4) == 0) {
        /* set clipboard: "%1i%4i%1i%s" */
        uint8_t id, marker;
        uint32_t seq;
        if (io_read_full(fd, &id, 1) != 1 || read_u32(fd, &seq) != 0 ||
            io_read_full(fd, &marker, 1) != 1 ||
            read_and_discard_string(fd) != 0) {
            return -1;
        }
        return 0;
    }
    if (memcmp(code, "CSEC", 4) == 0) {
        uint8_t on;
        if (io_read_full(fd, &on, 1) != 1) {
            return -1;
        }
        return 0; /* screensaver, ignore */
    }
    if (memcmp(code, "EICV", 4) == 0) {
        uint16_t maj, min;
        read_u16(fd, &maj);
        read_u16(fd, &min);
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

    LOG_DBG("ignoring unknown message %.4s\n", code);
    return -1;
}

int df_client_run(df_client *c, volatile sig_atomic_t *stop)
{
    int fd = c->fd;
    int result = 0;

    for (;;) {
        if (stop && *stop) {
            break;
        }

        /* Wait for the next message (with a timeout so a stop request can be
         * honored promptly). */
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
            continue; /* timeout, loop to check stop flag */
        }

        char code[4];
        int r = io_read_full(fd, code, 4);
        if (r == 0) {
            LOG_INFO("server closed the connection\n");
            result = 0;
            break;
        }
        if (r < 0) {
            LOG_ERR("read error: %s\n", strerror(errno));
            result = -1;
            break;
        }

        int h = handle_message(c, code);
        if (h < 0) {
            result = -1;
            break;
        }
        if (h > 0) {
            /* server asked us to close (CBYE) */
            result = 0;
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
