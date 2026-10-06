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
    CHECK(df_socket_init() == 0);
    df_socket fds[2];
#ifdef _WIN32
    /* Winsock has no socketpair(): connect two TCP sockets via loopback. */
    df_socket listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener != DF_INVALID_SOCKET);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(listen(listener, 1) == 0);
    int addrlen = sizeof(addr);
    CHECK(getsockname(listener, (struct sockaddr *)&addr, &addrlen) == 0);
    fds[0] = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fds[0] != DF_INVALID_SOCKET);
    CHECK(connect(fds[0], (struct sockaddr *)&addr, sizeof(addr)) == 0);
    fds[1] = accept(listener, NULL, NULL);
    CHECK(fds[1] != DF_INVALID_SOCKET);
    df_close_socket(listener);
#else
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
#endif
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

    /* Shut down the writer while keeping the peer open for an EOF check. */
#ifdef _WIN32
    CHECK(shutdown(fds[1], SD_SEND) == 0);
#else
    CHECK(shutdown(fds[1], SHUT_WR) == 0);
#endif
    CHECK(io_read_full(fds[0], wire, 1) == 0);
#ifndef _WIN32
    close(fds[1]);
    /* Keep SIGPIPE at its default: a regression terminates this test. */
    CHECK(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
    CHECK(send_packet(fds[0], "CNOP", 4) == -1);
    CHECK(errno == EPIPE);
#else
    /* Windows reports a socket error instead of raising SIGPIPE. */
    CHECK(shutdown(fds[0], SD_SEND) == 0);
    CHECK(send_packet(fds[0], "CNOP", 4) == -1);
    df_close_socket(fds[1]);
#endif
    df_close_socket(fds[0]);
    df_socket_cleanup();
    puts("PASS: packet framing, EOF and disconnected socket writes");
    return 0;
}
