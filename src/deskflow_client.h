/*
 * deskflow_client.h - minimal Deskflow protocol client (secondary screen).
 *
 * Implements just enough of the Deskflow (Synergy/Barrier) wire protocol for a
 * headless input sink: the handshake, screen-info exchange and the keyboard /
 * mouse event stream.  See deskflow's src/lib/deskflow/ProtocolTypes.h for the
 * authoritative protocol documentation this is modeled on.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DESKFLOW_OTG_DESKFLOW_CLIENT_H
#define DESKFLOW_OTG_DESKFLOW_CLIENT_H

#include <signal.h>
#include <stdint.h>

/* Event callbacks invoked from the client loop (must be non-blocking). */
typedef struct df_client_callbacks {
    /* Called once after the server accepts screen info and sends options. */
    void (*on_connected)(void *ud);
    void (*on_enter)(void *ud, int16_t x, int16_t y, uint32_t seq, uint16_t mask);
    void (*on_leave)(void *ud);
    void (*on_key_down)(void *ud, uint32_t keyid, uint16_t mask);
    void (*on_key_up)(void *ud, uint32_t keyid, uint16_t mask);
    void (*on_key_repeat)(void *ud, uint32_t keyid, uint16_t mask, int32_t count);
    void (*on_mouse_down)(void *ud, uint8_t button);
    void (*on_mouse_up)(void *ud, uint8_t button);
    void (*on_mouse_move)(void *ud, int16_t x, int16_t y);
    void (*on_mouse_rel_move)(void *ud, int16_t dx, int16_t dy);
    void (*on_mouse_wheel)(void *ud, int16_t x, int16_t y);
    void (*on_disconnected)(void *ud);

    /*
     * Optional periodic hook, invoked every time the event loop times out
     * waiting for data (every poll interval, ~500ms).  Return non-zero to
     * stop the loop cleanly.  Used to notice external conditions such as the
     * USB device being unplugged.
     */
    int (*on_idle)(void *ud);
} df_client_callbacks;

typedef struct df_client df_client;

/* Optional cancellation check for this single-threaded client, including
 * partial packet reads. Set before connecting; NULL restores CLI behaviour. */
void df_client_set_stop_check(int (*check)(void));

/*
 * Connect to a Deskflow server and exchange the initial hello. Screen acceptance
 * occurs later in df_client_run() and is reported through on_connected.
 *
 * `name` is the screen name this client reports (must match a screen in the
 * server configuration).  `screen_w`/`screen_h` describe the virtual screen
 * size reported to the server (use the Android display resolution).  Returns
 * NULL on failure.
 */
df_client *df_client_connect(const char *host, uint16_t port, const char *name,
                             int32_t screen_w, int32_t screen_h,
                             const df_client_callbacks *cbs, void *ud);

/*
 * Run the message loop until the server closes the connection, an error
 * occurs, or *stop (if non-NULL) becomes non-zero.  Returns 0 on clean
 * shutdown, non-zero otherwise.
 */
int df_client_run(df_client *c, volatile sig_atomic_t *stop);

/* Disconnect and free the client. */
void df_client_disconnect(df_client *c);

#endif /* DESKFLOW_OTG_DESKFLOW_CLIENT_H */
