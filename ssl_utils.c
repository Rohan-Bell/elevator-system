#include "shared.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

void send_looped_ssl(SSL *ssl, const void *buf, size_t sz) {
    const char *ptr = buf;
    size_t remain = sz;
    while (remain > 0) {
        int sent = SSL_write(ssl, ptr, remain);
        if (sent <= 0) {
            ERR_print_errors_fp(stderr);
            break;
        }
        ptr += sent;
        remain -= sent;
    }
}

char *receive_msg_ssl(SSL *ssl) {
    uint16_t nlen;
    int ret = SSL_read(ssl, &nlen, sizeof(nlen));
    if (ret != sizeof(nlen)) {
        return NULL;
    }
    uint16_t len = ntohs(nlen);
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    buf[len] = '\0';
    ret = SSL_read(ssl, buf, len);
    if (ret != len) {
        free(buf);
        return NULL;
    }
    return buf;
}

void send_message_ssl(SSL *ssl, const char *buf) {
    uint16_t len = htons(strlen(buf));
    send_looped_ssl(ssl, &len, sizeof(len));
    send_looped_ssl(ssl, buf, strlen(buf));
}

SSL_CTX *init_ssl_context(void) {
    SSL_CTX *ctx = NULL;

    // Initialize OpenSSL
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    // Create SSL context
    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    // Load certificate and private key
    if (SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (!SSL_CTX_check_private_key(ctx)) {
        fprintf(stderr, "Private key does not match certificate\n");
        SSL_CTX_free(ctx);
        return NULL;
    }

    return ctx;
}

SSL_CTX *init_ssl_client_context(void) {
    SSL_CTX *ctx = NULL;

    // Initialize OpenSSL
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    // Create SSL context
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    // For self-signed certificates, disable verification
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    return ctx;
}