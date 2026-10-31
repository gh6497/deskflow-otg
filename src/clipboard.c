/* Clipboard wire formats: Deskflow IClipboard and scrcpy control/device messages.
 * Independently implemented from protocol layouts; no OS clipboard access.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "clipboard.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#endif

uint32_t otg_clipboard_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
void otg_clipboard_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
bool otg_clipboard_valid_text(const char *text, size_t size)
{
    if ((!text && size) || size > OTG_CLIPBOARD_TEXT_MAX) return false;
    for (size_t i = 0; i < size;) {
        unsigned char b = (unsigned char)text[i++];
        if (!b) return false; /* Android/desktop string APIs cannot preserve NUL. */
        if (b < 0x80) continue;
        unsigned n;
        uint32_t v, min;
        if (b >= 0xc2 && b <= 0xdf) { n = 1; v = b & 31; min = 0x80; }
        else if (b >= 0xe0 && b <= 0xef) { n = 2; v = b & 15; min = 0x800; }
        else if (b >= 0xf0 && b <= 0xf4) { n = 3; v = b & 7; min = 0x10000; }
        else return false;
        if (n > size - i) return false;
        while (n--) {
            b = (unsigned char)text[i++];
            if ((b & 0xc0) != 0x80) return false;
            v = (v << 6) | (b & 63);
        }
        if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return false;
    }
    return true;
}
int otg_clipboard_decode(const uint8_t *data, size_t size, const char **text, size_t *length)
{
    if (size < 4) return -1;
    uint32_t count = otg_clipboard_be32(data);
    size_t pos = 4;
    int found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (size - pos < 8) return -1;
        uint32_t format = otg_clipboard_be32(data + pos);
        uint32_t n = otg_clipboard_be32(data + pos + 4);
        pos += 8;
        if (n > size - pos) return -1;
        if (format == 0) {
            if (found || !otg_clipboard_valid_text((const char *)data + pos, n)) return -1;
            *text = (const char *)data + pos; *length = n; found = 1;
        }
        pos += n;
    }
    return pos == size ? found : -1;
}
uint8_t *otg_clipboard_encode(const char *text, size_t size, size_t *length)
{
    if (!otg_clipboard_valid_text(text, size)) return NULL;
    uint8_t *out = malloc(size + 12);
    if (!out) return NULL;
    otg_clipboard_put32(out, 1); otg_clipboard_put32(out + 4, 0);
    otg_clipboard_put32(out + 8, (uint32_t)size);
    if (size) memcpy(out + 12, text, size);
    *length = size + 12;
    return out;
}
static uint64_t now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}
int otg_clipboard_channel_open(struct otg_clipboard_channel *c, intptr_t fd)
{
    memset(c, 0, sizeof(*c)); c->fd = fd;
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &nonblocking)) return -1;
#else
    int flags = fcntl((int)fd, F_GETFL, 0);
    if (flags < 0 || fcntl((int)fd, F_SETFL, flags | O_NONBLOCK)) return -1;
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt((int)fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one))) return -1;
#endif
#endif
    c->rx = malloc((1U << 18) + 5U);
    c->ready = c->rx != NULL;
    return c->ready ? 0 : -1;
}
void otg_clipboard_channel_close(struct otg_clipboard_channel *c)
{
    free(c->rx); free(c->tx); free(c->queued); free(c->last_set);
    memset(c, 0, sizeof(*c)); c->fd = -1;
}
bool otg_clipboard_channel_busy(const struct otg_clipboard_channel *c)
{
    return c->ready && (c->pending || c->queued || c->tx);
}
int otg_clipboard_channel_set(struct otg_clipboard_channel *c, const char *text, size_t size)
{
    if (!c->ready || !otg_clipboard_valid_text(text, size)) return -1;
    uint8_t *out = malloc(size + 14);
    if (!out) return -1;
    out[0] = 9; /* SET_CLIPBOARD, sequence filled when made in-flight */
    out[9] = 0; /* no SDK paste: real HID keyboard remains in charge */
    otg_clipboard_put32(out + 10, (uint32_t)size);
    if (size) memcpy(out + 14, text, size);
    free(c->queued); c->queued = out; c->queued_size = size + 14;
    return 0;
}
static bool would_block(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
int otg_clipboard_channel_poll(struct otg_clipboard_channel *c,
    void (*on_text)(void *, const char *, size_t), void *ud)
{
    if (!c->ready) return -1;
    if (c->pending && now_ms() >= c->deadline_ms) goto fail;
    if (!c->pending && c->queued) {
        c->tx = c->queued; c->tx_size = c->queued_size; c->tx_pos = 0;
        c->queued = NULL; c->queued_size = 0;
        free(c->last_set);
        c->last_set_size = c->tx_size - 14;
        c->last_set = malloc(c->last_set_size ? c->last_set_size : 1);
        if (!c->last_set) goto fail;
        if (c->last_set_size) memcpy(c->last_set, c->tx + 14, c->last_set_size);
        c->pending = ++c->sequence;
        otg_clipboard_put32(c->tx + 1, (uint32_t)(c->pending >> 32));
        otg_clipboard_put32(c->tx + 5, (uint32_t)c->pending);
        c->deadline_ms = now_ms() + 2000;
    }
    if (c->tx) {
        size_t remaining = c->tx_size - c->tx_pos;
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
#ifdef _WIN32
        int n = send((SOCKET)c->fd, (const char *)c->tx + c->tx_pos, (int)remaining, flags);
#else
        int n = (int)send((int)c->fd, c->tx + c->tx_pos, remaining, flags);
#endif
        if (n < 0 && !would_block()) goto fail;
        if (n == 0) goto fail;
        if (n > 0) {
            c->tx_pos += (size_t)n;
            if (c->tx_pos == c->tx_size) { free(c->tx); c->tx = NULL; }
        }
    }
    /* Bound work per tick so a noisy device cannot starve HID/Deskflow. */
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        size_t capacity = (1U << 18) + 5U - c->rx_size;
        if (!capacity) goto fail;
        if (capacity > 16384) capacity = 16384;
#ifdef _WIN32
        int n = recv((SOCKET)c->fd, (char *)c->rx + c->rx_size, (int)capacity, 0);
#else
        int n = (int)recv((int)c->fd, c->rx + c->rx_size, capacity, 0);
#endif
        if (n < 0) { if (would_block()) break; goto fail; }
        if (!n) goto fail;
        c->rx_size += (size_t)n;
        while (c->rx_size) {
            size_t length;
            if (c->rx[0] == 0) {
                if (c->rx_size < 5) break;
                uint32_t size = otg_clipboard_be32(c->rx + 1);
                if (size > (1U << 18) - 5U) goto fail;
                length = size + 5U;
                if (c->rx_size < length) break;
                if (!otg_clipboard_valid_text((const char *)c->rx + 5, size)) {
                    fprintf(stderr, "WARN: Android clipboard text invalid or exceeds size limit; not shared\n");
                } else {
                    bool echo = c->last_set && c->last_set_size == size &&
                                (!size || !memcmp(c->last_set, c->rx + 5, size));
                    if (!echo) {
                        free(c->last_set); c->last_set = NULL;
                        if (on_text) on_text(ud, (const char *)c->rx + 5, size);
                        /* A failed Deskflow send may disable/close this channel
                         * from the callback; do not touch its freed rx buffer. */
                        if (!c->ready) return -1;
                    }
                }
            } else if (c->rx[0] == 1) {
                length = 9;
                if (c->rx_size < length) break;
                uint64_t seq = (uint64_t)otg_clipboard_be32(c->rx + 1) << 32 |
                               otg_clipboard_be32(c->rx + 5);
                if (seq == c->pending && !c->tx) c->pending = 0;
            } else goto fail; /* No UHID devices are created by this client. */
            c->rx_size -= length;
            memmove(c->rx, c->rx + length, c->rx_size);
        }
    }
    return 0;
fail:
    otg_clipboard_channel_close(c);
    return -1;
}
