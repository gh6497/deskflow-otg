/* Deskflow clipboard framing/lifecycle + scrcpy control tests, loopback only.
 * Protocol references: Deskflow ProtocolTypes/IClipboard, scrcpy control_msg.
 * SPDX-License-Identifier: Apache-2.0 */
#include "../src/deskflow_client.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void pair(df_socket fds[2])
{
#ifdef _WIN32
    df_socket listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, (struct sockaddr *)&a, sizeof(a)) == 0);
    CHECK(listen(listener, 1) == 0);
    int length = sizeof(a);
    CHECK(getsockname(listener, (struct sockaddr *)&a, &length) == 0);
    fds[0] = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(fds[0], (struct sockaddr *)&a, sizeof(a)) == 0);
    fds[1] = accept(listener, NULL, NULL); df_close_socket(listener);
#else
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
#endif
    CHECK(socket_suppress_sigpipe(fds[0]) == 0);
    CHECK(socket_suppress_sigpipe(fds[1]) == 0);
}

static unsigned copies;
static char received[256];
static size_t received_size;
static void receive_text(void *ud, const char *text, size_t size)
{
    (void)ud;
    CHECK(size < sizeof(received));
    memcpy(received, text, size); received[size] = 0; received_size = size; ++copies;
}
static void dispatch(df_client *c, const char *code, const uint8_t *data, size_t size)
{
    struct msg m = {data, size, 0};
    CHECK(handle_message(c, code, &m) == 0);
}
static void check_code(df_socket fd, const char *code)
{
    uint8_t *packet = NULL;
    CHECK(recv_packet(fd, &packet) >= 4);
    CHECK(!memcmp(packet, code, 4)); free(packet);
}
static void codec(void)
{
    const char text[] = "hello\n\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80";
    size_t length, decoded_size;
    uint8_t *data = otg_clipboard_encode(text, sizeof(text) - 1, &length);
    const char *decoded;
    CHECK(data && length == sizeof(text) - 1 + 12);
    CHECK(otg_clipboard_decode(data, length, &decoded, &decoded_size) == 1);
    CHECK(decoded_size == sizeof(text) - 1 && !memcmp(decoded, text, decoded_size));
    CHECK(otg_clipboard_decode(data, length - 1, &decoded, &decoded_size) == -1);
    data[7] = 1; /* HTML only */
    CHECK(otg_clipboard_decode(data, length, &decoded, &decoded_size) == 0);
    free(data);
    CHECK(!otg_clipboard_valid_text("a\0b", 3));
    CHECK(!otg_clipboard_valid_text("\xc0\x80", 2));
    CHECK(!otg_clipboard_valid_text("\xed\xa0\x80", 3));
    CHECK(!otg_clipboard_valid_text("\xf4\x90\x80\x80", 4));
    CHECK(!otg_clipboard_valid_text("\xe4\xb8", 2));
    CHECK(!otg_clipboard_valid_text("x", OTG_CLIPBOARD_TEXT_MAX + 1U));
    data = otg_clipboard_encode("", 0, &length);
    CHECK(data && otg_clipboard_decode(data, length, &decoded, &decoded_size) == 1 && !decoded_size);
    free(data);
}
static void deskflow(void)
{
    df_socket fds[2]; pair(fds);
    df_client c = {0};
    c.fd = fds[0]; c.proto_minor = 8; c.clipboard_server = true;
    c.clipboard_limit = OTG_CLIPBOARD_WIRE_MAX; c.clipboard_seq = 42;
    c.cbs.on_clipboard = receive_text;
    df_client_enable_clipboard(&c, true);
    c.clipboard_active = true;
    CHECK(df_client_set_clipboard(&c, "phone", 5) == 0);
    uint8_t *packet = NULL;
    CHECK(recv_packet(fds[1], &packet) == 9);
    CHECK(!memcmp(packet, "CCLP", 4) && packet[4] == 0 && otg_clipboard_be32(packet + 5) == 42);
    free(packet);
    CHECK(df_socket_wait_timeout(fds[1], 0) == 0); /* Active: no body until leave. */
    dispatch(&c, "COUT", NULL, 0);
    check_code(fds[1], "DCLP"); check_code(fds[1], "DCLP"); check_code(fds[1], "DCLP");
    CHECK(c.clipboard_local == NULL);
    dispatch(&c, "COUT", NULL, 0);
    CHECK(df_socket_wait_timeout(fds[1], 0) == 0); /* Unchanged not resent. */
    CHECK(df_client_set_clipboard(&c, "background", 10) == 0);
    check_code(fds[1], "CCLP");
    check_code(fds[1], "DCLP"); check_code(fds[1], "DCLP"); check_code(fds[1], "DCLP");
    c.clipboard_active = true;
    CHECK(df_client_set_clipboard(&c, "superseded", 10) == 0);
    check_code(fds[1], "CCLP");
    const uint8_t grab[] = {0, 0, 0, 0, 0};
    dispatch(&c, "CCLP", grab, sizeof(grab));
    dispatch(&c, "COUT", NULL, 0);
    CHECK(!c.clipboard_owned && df_socket_wait_timeout(fds[1], 0) == 0);

    size_t length;
    uint8_t *data = otg_clipboard_encode("computer", 8, &length);
    char size[32]; int n = snprintf(size, sizeof(size), "%zu", length);
    CHECK(!clipboard_receive(&c, 0, 0, 1, (const uint8_t *)size, (size_t)n));
    CHECK(!clipboard_receive(&c, 0, 0, 2, data, 3));
    CHECK(!clipboard_receive(&c, 0, 0, 2, data + 3, length - 3));
    CHECK(!clipboard_receive(&c, 0, 0, 3, NULL, 0));
    CHECK(copies == 1 && !strcmp(received, "computer"));
    /* Another device's partial transfer and ownership must remain independent. */
    df_client other = {0};
    other.proto_minor = 8; other.clipboard_server = true;
    other.clipboard_limit = OTG_CLIPBOARD_WIRE_MAX;
    other.cbs.on_clipboard = receive_text;
    df_client_enable_clipboard(&other, true);
    CHECK(!clipboard_receive(&other, 0, 0, 1, (const uint8_t *)size, (size_t)n));
    CHECK(!clipboard_receive(&other, 0, 0, 2, data, 4));
    CHECK(!clipboard_receive(&c, 0, 0, 1, (const uint8_t *)size, (size_t)n));
    CHECK(!clipboard_receive(&other, 0, 0, 2, data + 4, length - 4));
    CHECK(!clipboard_receive(&other, 0, 0, 3, NULL, 0));
    CHECK(copies == 2 && c.clipboard_rx[0].active);
    clipboard_clear(&other); clipboard_clear(&c); copies = 1;
    /* Selection clipboard, wrong sequences, malformed and oversize transfers. */
    CHECK(!clipboard_receive(&c, 1, 0, 1, (const uint8_t *)size, (size_t)n));
    CHECK(!clipboard_receive(&c, 1, 0, 2, data, length));
    CHECK(!clipboard_receive(&c, 1, 0, 3, NULL, 0));
    CHECK(copies == 1);
    CHECK(!clipboard_receive(&c, 0, 0, 1, (const uint8_t *)size, (size_t)n));
    CHECK(!clipboard_receive(&c, 0, 1, 2, data, length));
    CHECK(!clipboard_receive(&c, 0, 0, 3, NULL, 0));
    CHECK(copies == 1);
    c.clipboard_limit = 16;
    CHECK(!clipboard_receive(&c, 0, 0, 1, (const uint8_t *)size, (size_t)n));
    CHECK(!c.clipboard_rx[0].data);
    CHECK(!clipboard_receive(&c, 0, 0, 2, data, length));
    CHECK(!clipboard_receive(&c, 0, 0, 3, NULL, 0));
    CHECK(copies == 1);
    free(data);
    uint8_t options[] = {0,0,0,2,'C','L','P','S',0,0,0,0};
    dispatch(&c, "DSOP", options, sizeof(options));
    CHECK(!df_client_clipboard_enabled(&c));
    CHECK(!df_client_set_clipboard(&c, "ignored", 7));
    CHECK(df_socket_wait_timeout(fds[1], 0) == 0);
    dispatch(&c, "CROP", NULL, 0);
    CHECK(df_client_clipboard_enabled(&c));
    options[11] = 1; memcpy(options + 4, "CLSZ", 4);
    dispatch(&c, "DSOP", options, sizeof(options));
    CHECK(c.clipboard_limit == 1024);
    clipboard_clear(&c);
    df_close_socket(fds[0]); df_close_socket(fds[1]);
}
static void send_ack(df_socket fd, uint64_t seq)
{
    uint8_t ack[9] = {1};
    otg_clipboard_put32(ack + 1, (uint32_t)(seq >> 32));
    otg_clipboard_put32(ack + 5, (uint32_t)seq);
    CHECK(io_write_full(fd, ack, sizeof(ack)) == 0);
}
static void close_on_text(void *ud, const char *text, size_t size)
{
    (void)text; (void)size;
    otg_clipboard_channel_close(ud);
}
static void control(void)
{
    df_socket fds[2]; pair(fds);
    struct otg_clipboard_channel c;
    CHECK(!otg_clipboard_channel_open(&c, (intptr_t)fds[0]));
    CHECK(!otg_clipboard_channel_set(&c, "abc", 3));
    CHECK(otg_clipboard_channel_busy(&c));
    CHECK(!otg_clipboard_channel_poll(&c, receive_text, NULL));
    uint8_t out[17]; CHECK(io_read_full(fds[1], out, sizeof(out)) == 1);
    CHECK(out[0] == 9 && out[9] == 0 && otg_clipboard_be32(out + 10) == 3);
    CHECK(!memcmp(out + 14, "abc", 3));
    uint64_t seq = c.pending;
    send_ack(fds[1], seq + 1);
    CHECK(!otg_clipboard_channel_poll(&c, receive_text, NULL));
    CHECK(otg_clipboard_channel_busy(&c));
    /* Coalesce queued sets without replacing a partially transmitted request. */
    CHECK(!otg_clipboard_channel_set(&c, "old", 3));
    CHECK(!otg_clipboard_channel_set(&c, "new", 3));
    send_ack(fds[1], seq);
    CHECK(!otg_clipboard_channel_poll(&c, receive_text, NULL));
    CHECK(otg_clipboard_channel_busy(&c));
    CHECK(!otg_clipboard_channel_poll(&c, receive_text, NULL));
    CHECK(io_read_full(fds[1], out, sizeof(out)) == 1 && !memcmp(out + 14, "new", 3));
    uint8_t ack[9] = {1}; otg_clipboard_put32(ack + 5, (uint32_t)c.pending);
    CHECK(!io_write_full(fds[1], ack, 4));
    CHECK(!otg_clipboard_channel_poll(&c, receive_text, NULL));
    CHECK(otg_clipboard_channel_busy(&c));
    CHECK(!io_write_full(fds[1], ack + 4, 5));
    CHECK(!otg_clipboard_channel_poll(&c, receive_text, NULL));
    CHECK(!otg_clipboard_channel_busy(&c));
    const uint8_t text[] = {0,0,0,0,3,'n','e','w',0,0,0,0,3,'p','h','o'};
    copies = 0;
    CHECK(!io_write_full(fds[1], text, 2));
    CHECK(!otg_clipboard_channel_poll(&c, receive_text, NULL));
    CHECK(!io_write_full(fds[1], text + 2, sizeof(text) - 2));
    CHECK(!otg_clipboard_channel_poll(&c, receive_text, NULL));
    CHECK(copies == 1 && received_size == 3 && !strcmp(received, "pho")); /* own echo dropped */
    CHECK(!otg_clipboard_channel_set(&c, "timeout", 7));
    CHECK(!otg_clipboard_channel_poll(&c, receive_text, NULL));
    c.deadline_ms = 0;
    CHECK(otg_clipboard_channel_poll(&c, receive_text, NULL) == -1 && !c.ready);
    otg_clipboard_channel_close(&c); /* Idempotent; descriptors belong to session. */
    df_close_socket(fds[0]); df_close_socket(fds[1]);
    pair(fds);
    CHECK(!otg_clipboard_channel_open(&c, (intptr_t)fds[0]));
    const uint8_t huge[] = {0,0xff,0xff,0xff,0xff};
    CHECK(!io_write_full(fds[1], huge, sizeof(huge)));
    CHECK(otg_clipboard_channel_poll(&c, receive_text, NULL) == -1);
    df_close_socket(fds[0]); df_close_socket(fds[1]);
    pair(fds);
    CHECK(!otg_clipboard_channel_open(&c, (intptr_t)fds[0]));
    df_close_socket(fds[1]);
    CHECK(otg_clipboard_channel_poll(&c, receive_text, NULL) == -1);
    df_close_socket(fds[0]);
    pair(fds);
    CHECK(!otg_clipboard_channel_open(&c, (intptr_t)fds[0]));
    const uint8_t callback_text[] = {0,0,0,0,1,'x'};
    CHECK(!io_write_full(fds[1], callback_text, sizeof(callback_text)));
    CHECK(otg_clipboard_channel_poll(&c, close_on_text, &c) == -1);
    df_close_socket(fds[0]); df_close_socket(fds[1]);
}
int main(void)
{
    CHECK(!df_socket_init());
    codec(); deskflow(); control();
    df_socket_cleanup();
    puts("PASS: UTF-8, Deskflow ownership/chunks/options, scrcpy ACKs/echo/timeout/EOF");
    return 0;
}
