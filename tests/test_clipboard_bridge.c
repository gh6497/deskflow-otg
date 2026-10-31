/* Real main callbacks + bridge/HID with in-memory AOA and socketpair control.
 * No USB or Bluetooth operations. SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#define main otg_program_main
#include "../src/main.c"
#undef main
#include "../src/deskflow_client.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned keyboard_count;
static uint8_t keyboards[128][8];
bool aoa_hid_init(struct aoa_hid *a) { memset(a, 0, sizeof(*a)); return true; }
bool aoa_hid_open(struct aoa_hid *a, const char *serial) { (void)a; (void)serial; return true; }
void aoa_hid_destroy(struct aoa_hid *a) { (void)a; }
bool aoa_hid_register(struct aoa_hid *a, uint16_t id, const uint8_t *desc, uint16_t size)
{ (void)a; (void)id; return desc && size; }
bool aoa_hid_send(struct aoa_hid *a, uint16_t id, const uint8_t *data, uint16_t size)
{
    (void)a;
    if (id == 1) {
        CHECK(size == 8 && keyboard_count < 128);
        memcpy(keyboards[keyboard_count++], data, 8);
    }
    return true;
}
bool aoa_hid_poll(struct aoa_hid *a) { return !a->disconnected; }
bool aoa_hid_is_disconnected(const struct aoa_hid *a) { return a->disconnected; }
static void ack(int fd, struct otg_clipboard_channel *channel)
{
    uint8_t data[9] = {1};
    otg_clipboard_put32(data + 5, (uint32_t)channel->pending);
    CHECK(!io_write_full(fd, data, sizeof(data)));
}
static void sent(int fd, size_t text_size)
{
    uint8_t data[100];
    CHECK(io_read_full(fd, data, 14 + text_size) == 1);
    CHECK(data[0] == 9 && data[9] == 0);
}
int main(void)
{
    app_state s = {0};
    CHECK(!otg_bridge_open(&s.bridge, "fake", 1080, 1920, OTG_MOUSE_ABSOLUTE));
    df_client c = {0};
    c.proto_minor = 8; c.clipboard_server = true; c.clipboard_limit = OTG_CLIPBOARD_WIRE_MAX;
    c.cbs.on_clipboard_enabled = cb_clipboard_enabled; c.ud = &s; s.client = &c;
    df_client_enable_clipboard(&c, true);
    int fds[2]; CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    CHECK(!otg_clipboard_channel_open(&s.audio.clipboard, fds[0]));
    cb_enter(&s, 100, 200, 42, 0);
    cb_key_down(&s, 0xefe3, 0); /* Ctrl already held as the desktop clip arrives. */
    cb_clipboard(&s, "hello", 5);
    unsigned before = keyboard_count;
    cb_key_down(&s, 'v', 0); cb_key_up(&s, 'v', 0); cb_key_up(&s, 0xefe3, 0);
    CHECK(s.held_key_count == 3 && keyboard_count == before);
    clipboard_poll(&s); sent(fds[1], 5);
    CHECK(keyboard_count == before); /* Sending SET is not enough: must await ACK. */
    ack(fds[1], &s.audio.clipboard); clipboard_poll(&s);
    CHECK(!s.held_key_count && keyboard_count == before + 3);
    CHECK(keyboards[before][0] == 1 && keyboards[before][2] == 0x19);
    CHECK(keyboards[before + 1][0] == 1 && keyboards[before + 1][2] == 0);
    CHECK(keyboards[before + 2][0] == 0);

    /* Leave while SET is pending: discard queued keys, never paste next entry. */
    cb_clipboard(&s, "next", 4); cb_key_down(&s, 'v', 0);
    cb_leave(&s); sent(fds[1], 4);
    CHECK(!s.held_key_count && !s.bridge.active);
    before = keyboard_count;
    ack(fds[1], &s.audio.clipboard); clipboard_poll(&s);
    CHECK(keyboard_count == before);
    cb_enter(&s, 50, 60, 43, 0);

    /* Server disabling sharing cancels the key barrier and preserves HID. */
    cb_key_down(&s, 0xefe3, 0);
    cb_clipboard(&s, "disabled", 8); cb_key_down(&s, 'v', 0);
    df_client_enable_clipboard(&c, false);
    CHECK(!s.held_key_count && !s.bridge.hid_mods && !s.audio.clipboard.queued);
    cb_key_down(&s, 'a', 0); cb_key_up(&s, 'a', 0);
    df_client_enable_clipboard(&c, true);

    /* ACK timeout releases held modifiers, without changing pointer/drag state. */
    cb_mouse_down(&s, 1);
    cb_key_down(&s, 0xefe3, 0);
    cb_clipboard(&s, "timeout", 7); cb_key_down(&s, 'v', 0);
    clipboard_poll(&s); sent(fds[1], 7);
    s.audio.clipboard.deadline_ms = 0;
    clipboard_poll(&s);
    CHECK(s.clipboard_error && !s.held_key_count && !s.bridge.hid_mods);
    CHECK(s.bridge.active && s.bridge.mouse_buttons == 1);
    CHECK(!df_client_clipboard_enabled(&c));
    before = keyboard_count;
    cb_key_down(&s, 'b', 0); cb_key_up(&s, 'b', 0);
    CHECK(keyboard_count == before + 2);
    s.clipboard_error = false;
    CHECK(!otg_clipboard_channel_open(&s.audio.clipboard, fds[0]));
    df_client_enable_clipboard(&c, true);
    cb_key_down(&s, 0xefe3, 0);
    cb_clipboard(&s, "overflow", 8);
    for (unsigned i = 0; i < 64; ++i) cb_key_down(&s, 'a', 0);
    CHECK(s.held_key_count == 64);
    cb_key_down(&s, 'a', 0);
    CHECK(s.clipboard_error && !s.held_key_count && !s.bridge.hid_mods);
    CHECK(s.bridge.active && s.bridge.mouse_buttons == 1);
    close(fds[0]); close(fds[1]);
    clipboard_clear(&c); otg_bridge_close(&s.bridge);
    puts("PASS: main/HID ACK barrier, ordered releases, leave cancellation and failure isolation");
    return 0;
}
