/* Regression tests for Deskflow TLS fingerprint handling.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../src/deskflow_client.c"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <openssl/rsa.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    const char *compact =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const char *colon =
        "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:"
        "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF";
    const char *database =
        "v2:sha256:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    uint8_t digest[32];

    CHECK(tls_parse_fingerprint(compact, digest) == 0);
    CHECK(tls_fingerprint_matches(digest, compact) == 1);
    CHECK(tls_fingerprint_matches(digest, colon) == 1);
    CHECK(tls_fingerprint_matches(digest, database) == 1);
    CHECK(tls_parse_fingerprint("0011", digest) != 0);
    CHECK(tls_parse_fingerprint("zz1122", digest) != 0);
    digest[0] ^= 1;
    CHECK(tls_fingerprint_matches(digest, compact) == 0);

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    /* Exercise the complete non-blocking TLS handshake and the first
     * Deskflow packet. The self-signed certificate mirrors Deskflow's normal
     * deployment; trust is established only by the SHA-256 pin above. */
    EVP_PKEY_CTX *key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    CHECK(key_context != NULL);
    CHECK(EVP_PKEY_keygen_init(key_context) == 1);
    CHECK(EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) == 1);
    EVP_PKEY *key = NULL;
    CHECK(EVP_PKEY_keygen(key_context, &key) == 1);
    EVP_PKEY_CTX_free(key_context);

    X509 *certificate = X509_new();
    CHECK(certificate != NULL);
    CHECK(X509_set_version(certificate, 2) == 1);
    CHECK(ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) == 1);
    CHECK(X509_gmtime_adj(X509_get_notBefore(certificate), 0) != NULL);
    CHECK(X509_gmtime_adj(X509_get_notAfter(certificate), 3600) != NULL);
    CHECK(X509_set_pubkey(certificate, key) == 1);
    X509_NAME *subject = X509_get_subject_name(certificate);
    CHECK(subject != NULL);
    CHECK(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                     (const unsigned char *)"deskflow-test", -1, -1, 0) == 1);
    CHECK(X509_set_issuer_name(certificate, subject) == 1);
    CHECK(X509_sign(certificate, key, EVP_sha256()) > 0);
    unsigned int fingerprint_size = 0;
    CHECK(X509_digest(certificate, EVP_sha256(), digest, &fingerprint_size) == 1);
    CHECK(fingerprint_size == 32);
    char fingerprint[65];
    for (unsigned i = 0; i < 32; ++i)
        snprintf(fingerprint + i * 2, 3, "%02x", (unsigned)digest[i]);
    fingerprint[64] = '\0';

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t address_size = sizeof(address);
    CHECK(getsockname(listener, (struct sockaddr *)&address, &address_size) == 0);
    CHECK(listen(listener, 1) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        int accepted_socket = accept(listener, NULL, NULL);
        if (accepted_socket < 0) _exit(2);
        SSL_CTX *server_context = SSL_CTX_new(TLS_server_method());
        int ok = server_context &&
                 SSL_CTX_set_min_proto_version(server_context, TLS1_2_VERSION) == 1 &&
                 SSL_CTX_use_certificate(server_context, certificate) == 1 &&
                 SSL_CTX_use_PrivateKey(server_context, key) == 1 &&
                 SSL_CTX_check_private_key(server_context) == 1;
        SSL *server_ssl = ok ? SSL_new(server_context) : NULL;
        ok = ok && server_ssl &&
                 SSL_set_fd(server_ssl, accepted_socket) == 1 &&
                 SSL_accept(server_ssl) == 1;
        const uint8_t hello[] = {0, 0, 0, 11, 'B', 'a', 'r', 'r', 'i', 'e', 'r', 0, 1, 0, 8};
        if (ok && SSL_write(server_ssl, hello, sizeof(hello)) != (int)sizeof(hello)) ok = 0;
        uint8_t response[64];
        size_t response_size = 0;
        while (ok && response_size < 26) {
            int received = SSL_read(server_ssl, response + response_size,
                                    (int)(sizeof(response) - response_size));
            if (received <= 0) { ok = 0; break; }
            response_size += (size_t)received;
        }
        const uint8_t expected_response[] = {
            0, 0, 0, 22, 'B', 'a', 'r', 'r', 'i', 'e', 'r', 0, 1, 0, 8,
            0, 0, 0, 7, 'a', 'n', 'd', 'r', 'o', 'i', 'd'
        };
        if (ok && (response_size < sizeof(expected_response) ||
                   memcmp(response, expected_response, sizeof(expected_response)) != 0)) ok = 0;
        if (server_ssl) { SSL_shutdown(server_ssl); SSL_free(server_ssl); }
        if (server_context) SSL_CTX_free(server_context);
        close(accepted_socket);
        close(listener);
        _exit(ok ? 0 : 3);
    }
    close(listener);
    df_client_callbacks callbacks = {0};
    df_client_tls_options options = {
        .enabled = true, .certificate = NULL, .fingerprint = fingerprint, .fingerprint_file = NULL
    };
    df_client *client = df_client_connect_ex("127.0.0.1", ntohs(address.sin_port), "android",
                                             1080, 1920, &callbacks, NULL, &options);
    CHECK(client != NULL);
    df_client_disconnect(client);
    int child_status = 0;
    CHECK(waitpid(child, &child_status, 0) == child);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    X509_free(certificate);
    EVP_PKEY_free(key);
#endif
    puts("PASS: TLS SHA-256 fingerprint parsing and pinning");
    return 0;
}
