/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

/* TLS on AROS, over OpenSSL: the library AROS contrib builds, linked into
 * Pkg statically so that a machine reading a channel installs nothing.
 *
 * The bytes go through a BIO of Pkg's own, which calls bsdsocket's send and
 * recv: OpenSSL's socket BIO reaches for a file descriptor, and a bsdsocket
 * handle is not one. The sockets are blocking, so a short read or write is
 * the end of the connection and never a retry.
 *
 * Verification is on and has no switch: unknown issuer, wrong host and a
 * certificate outside its dates all end the transfer, each with its own
 * sentence. */

#include "pkg_tls.h"
#include "pkg.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <proto/bsdsocket.h>
#include <sys/socket.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

/* The authorities, made by the build scripts from third_party/cacert. */
extern const unsigned char pkg_ca_pem[];
extern const unsigned int pkg_ca_pem_len;

static SSL_CTX *ctx;
static SSL *ssl;
static BIO_METHOD *bio_method;
static int bio_socket;

static int sock_write(BIO *b, const char *buf, int len)
{
    int n = (int)send(bio_socket, (APTR)buf, (LONG)len, 0);
    BIO_clear_retry_flags(b);
    return n > 0 ? n : -1;
}

static int sock_read(BIO *b, char *buf, int len)
{
    int n = (int)recv(bio_socket, buf, (LONG)len, 0);
    BIO_clear_retry_flags(b);
    /* 0 is the server's close, which the caller reads as the end of a
     * body; only a negative return is a failure. */
    return n >= 0 ? n : -1;
}

static long sock_ctrl(BIO *b, int cmd, long larg, void *parg)
{
    (void)b; (void)larg; (void)parg;
    return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

/* Every certificate of the built-in bundle into the context's store. */
static int load_builtin_cas(char *err, size_t errlen)
{
    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    BIO *mem = BIO_new_mem_buf(pkg_ca_pem, (int)pkg_ca_pem_len);
    int n = 0;
    X509 *c;

    if (mem == NULL) { snprintf(err, errlen, "out of memory reading the built-in certificates"); return -1; }
    while ((c = PEM_read_bio_X509(mem, NULL, NULL, NULL)) != NULL) {
        if (X509_STORE_add_cert(store, c))
            n++;
        X509_free(c);
    }
    ERR_clear_error();                          /* the end of the file is not an error */
    BIO_free(mem);
    if (n == 0) {
        snprintf(err, errlen, "this Pkg was built without certificate authorities: rebuild it, "
                 "or name a bundle with PKG_CAFILE");
        return -1;
    }
    return 0;
}

/* What the machine believes the date is, for a certificate it thinks is not
 * yet valid: a wrong clock is the likelier cause on a machine with no
 * battery. */
static void today(char *out, size_t n)
{
    time_t t = time(NULL);
    struct tm *g = gmtime(&t);
    if (g == NULL || strftime(out, n, "%Y-%m-%d", g) == 0)
        snprintf(out, n, "an unknown date");
}

static void verify_words(long v, const char *host, char *err, size_t errlen)
{
    char now[32];
    switch (v) {
    case X509_V_ERR_CERT_HAS_EXPIRED:
        today(now, sizeof now);
        snprintf(err, errlen, "%s sends a certificate that expired: wait for the server to renew it, or, "
                 "if this machine's clock is wrong, set the date, which it believes is %s", host, now);
        break;
    case X509_V_ERR_CERT_NOT_YET_VALID:
        today(now, sizeof now);
        snprintf(err, errlen, "%s sends a certificate that is not valid yet: this machine's clock is "
                 "probably wrong, and it believes the date is %s", host, now);
        break;
    case X509_V_ERR_HOSTNAME_MISMATCH:
        snprintf(err, errlen, "the certificate %s sends is made out to another name: name the channel with "
                 "the address the certificate carries", host);
        break;
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        snprintf(err, errlen, "%s sends a certificate from an authority this Pkg does not know: if it is "
                 "your own authority, name its file with PKG_CAFILE", host);
        break;
    default:
        snprintf(err, errlen, "the certificate %s sends cannot be checked: %s", host,
                 X509_verify_cert_error_string(v));
        break;
    }
}

int pkg_tls_open(int sock, const char *host, char *err, size_t errlen)
{
    const char *cafile = getenv("PKG_CAFILE");
    BIO *bio;
    int ok;

    pkg_tls_close();
    if (ctx == NULL) {
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == NULL) { snprintf(err, errlen, "TLS does not start on this machine"); return -1; }
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        if (cafile != NULL && cafile[0] != '\0') {
            if (!SSL_CTX_load_verify_file(ctx, cafile)) {
                snprintf(err, errlen, "cannot read the certificates of PKG_CAFILE (%s): name a file of "
                         "PEM certificates, or unset it to use the ones built in", cafile);
                SSL_CTX_free(ctx); ctx = NULL;
                return -1;
            }
        } else if (load_builtin_cas(err, errlen) != 0) {
            SSL_CTX_free(ctx); ctx = NULL;
            return -1;
        }
    }
    if (bio_method == NULL) {
        bio_method = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "pkg bsdsocket");
        if (bio_method == NULL) { snprintf(err, errlen, "TLS does not start on this machine"); return -1; }
        BIO_meth_set_write(bio_method, sock_write);
        BIO_meth_set_read(bio_method, sock_read);
        BIO_meth_set_ctrl(bio_method, sock_ctrl);
    }
    ssl = SSL_new(ctx);
    bio = ssl != NULL ? BIO_new(bio_method) : NULL;
    if (bio == NULL) {
        if (ssl) { SSL_free(ssl); ssl = NULL; }
        snprintf(err, errlen, "out of memory starting TLS with %s", host);
        return -1;
    }
    bio_socket = sock;
    BIO_set_init(bio, 1);
    /* one BIO reads and writes, and SSL_set_bio takes it over */
    SSL_set_bio(ssl, bio, bio);
    /* The name the server is asked for and the name the certificate must
     * carry: the same one, always. An address written as numbers is no name:
     * it is not sent as the server name, and the certificate has to hold the
     * address itself. */
    if (host[strspn(host, "0123456789.")] == '\0') {
        ok = SSL_set1_ipaddr(ssl, host);
    } else {
        SSL_set_tlsext_host_name(ssl, host);
        ok = SSL_set1_dnsname(ssl, host);
    }
    if (!ok) {
        snprintf(err, errlen, "%s is not a name a certificate can be checked against", host);
        pkg_tls_close();
        return -1;
    }
    if (SSL_connect(ssl) != 1) {
        long v = SSL_get_verify_result(ssl);
        if (v != X509_V_OK)
            verify_words(v, host, err, errlen);
        else
            snprintf(err, errlen, "%s does not answer in TLS: check that the address is right, and that "
                     "nothing on the way is rewriting the connection", host);
        ERR_clear_error();
        pkg_tls_close();
        return -1;
    }
    return 0;
}

ssize_t pkg_tls_read(void *buf, size_t n)
{
    int r;
    if (ssl == NULL) return -1;
    r = SSL_read(ssl, buf, (int)(n > 0x7fffffffu ? 0x7fffffffu : n));
    if (r > 0) return r;
    /* the server's own close is the end of the body, not a failure */
    return SSL_get_error(ssl, r) == SSL_ERROR_ZERO_RETURN ? 0 : (r == 0 ? 0 : -1);
}

ssize_t pkg_tls_write(const void *buf, size_t n)
{
    int r;
    if (ssl == NULL) return -1;
    r = SSL_write(ssl, buf, (int)(n > 0x7fffffffu ? 0x7fffffffu : n));
    return r > 0 ? r : -1;
}

void pkg_tls_close(void)
{
    if (ssl != NULL) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ssl = NULL;
        ERR_clear_error();
    }
}

int pkg_tls_active(void)
{
    return ssl != NULL;
}
