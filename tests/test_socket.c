/* Exercise internal packet I/O, including Darwin's SIGPIPE handling.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../src/deskflow_client.c"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

int main(void)
{
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(socket_suppress_sigpipe(fds[0]) == 0);
    CHECK(socket_suppress_sigpipe(fds[1]) == 0);

    CHECK(send_packet(fds[0], "CALV", 4) == 0);
    uint8_t wire[8];
    const uint8_t expected[] = {0, 0, 0, 4, 'C', 'A', 'L', 'V'};
    CHECK(io_read_full(fds[1], wire, sizeof(wire)) == 1);
    CHECK(memcmp(wire, expected, sizeof(wire)) == 0);

    /* Partial writes and back-to-back packets preserve their boundaries. */
    CHECK(io_write_full(fds[1], expected, 2) == 0);
    CHECK(io_write_full(fds[1], expected + 2, sizeof(expected) - 2) == 0);
    CHECK(send_packet(fds[1], "CNOP", 4) == 0);
    uint8_t *payload = NULL;
    CHECK(recv_packet(fds[0], &payload) == 4);
    CHECK(memcmp(payload, "CALV", 4) == 0);
    free(payload);
    CHECK(recv_packet(fds[0], &payload) == 4);
    CHECK(memcmp(payload, "CNOP", 4) == 0);
    free(payload);

    close(fds[1]);
    CHECK(io_read_full(fds[0], wire, 1) == 0);
    /* Keep SIGPIPE at its default: a regression terminates this test. */
    CHECK(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
    CHECK(send_packet(fds[0], "CNOP", 4) == -1);
    CHECK(errno == EPIPE);
    close(fds[0]);
    puts("PASS: packet framing, EOF and disconnected socket writes");
    return 0;
}
