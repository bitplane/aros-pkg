/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 */

/* TLS on AROS, over Mbed TLS: a small TLS library made to be carried inside
 * a program, compiled into Pkg from third_party/mbedtls so that a machine
 * reading a channel installs nothing, on every CPU AROS runs on.
 *
 * Mbed TLS asks for three things of the system, and gets them here: the
 * bytes go through bsdsocket's send and recv, the randomness comes from
 * getentropy(), which AROS answers from entropy.resource, and the date, for
 * a certificate's validity, from time(). The sockets are blocking, so a
 * short read or write is the end of the connection and never a retry.
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
#include <unistd.h>

#include <proto/bsdsocket.h>
#include <sys/socket.h>

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>        /* the error codes a transport returns */
#include <mbedtls/platform_time.h>
#include <psa/crypto.h>

/* The authorities, made by the build scripts from third_party/cacert: a C
 * string, so its terminating NUL is there, as the PEM parser wants. */
extern const unsigned char pkg_ca_pem[];
extern const unsigned int pkg_ca_pem_len;

static mbedtls_ssl_context ssl;
static mbedtls_ssl_config conf;
static mbedtls_x509_crt cas;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static int ready;                       /* conf, cas and drbg are set up */
static int active;                      /* between open and close */
static int the_socket;

/* ---- what Mbed TLS asks of the system ----------------------------------- */

/* MBEDTLS_ENTROPY_HARDWARE_ALT: the one entropy source. */
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
    size_t at = 0;
    (void)data;
    while (at < len) {                  /* getentropy gives 256 bytes at most */
        size_t n = len - at > 256 ? 256 : len - at;
        if (getentropy(out + at, n) != 0)
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        at += n;
    }
    *olen = len;
    return 0;
}

/* MBEDTLS_PLATFORM_MS_TIME_ALT: only differences are used, for timeouts
 * Pkg does not set; seconds are enough. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)time(NULL) * 1000;
}

static int sock_send(void *c, const unsigned char *buf, size_t len)
{
    int n;
    (void)c;
    n = (int)send(the_socket, (APTR)buf, (LONG)(len > 0x7fffffffu ? 0x7fffffffu : len), 0);
    return n > 0 ? n : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int sock_recv(void *c, unsigned char *buf, size_t len)
{
    int n;
    (void)c;
    n = (int)recv(the_socket, buf, (LONG)(len > 0x7fffffffu ? 0x7fffffffu : len), 0);
    /* 0 is the server's close, which Mbed TLS reports as the end of the
     * connection; only a negative return is a failure. */
    return n >= 0 ? n : MBEDTLS_ERR_NET_RECV_FAILED;
}

/* ---- the authorities ------------------------------------------------------ */

/* A PEM file whole, NUL-terminated as the parser wants. */
static unsigned char *read_pem(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    long size;
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && size < (8l << 20)
        && fseek(f, 0, SEEK_SET) == 0 && (buf = (unsigned char *)malloc((size_t)size + 1)) != NULL) {
        if (fread(buf, 1, (size_t)size, f) == (size_t)size) {
            buf[size] = '\0';
            *len = (size_t)size + 1;
        } else {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    return buf;
}

/* PKG_CAFILE, or the bundle built in. A bundle may hold a certificate this
 * build cannot read (a curve it leaves out): the others still count, and the
 * parser's return says how many were skipped. None read at all is a failure. */
static int load_cas(char *err, size_t errlen)
{
    const char *cafile = getenv("PKG_CAFILE");
    int rc;

    mbedtls_x509_crt_init(&cas);
    if (cafile != NULL && cafile[0] != '\0') {
        size_t len = 0;
        unsigned char *pem = read_pem(cafile, &len);
        rc = pem != NULL ? mbedtls_x509_crt_parse(&cas, pem, len) : -1;
        free(pem);
        if (rc < 0 || cas.version == 0) {
            snprintf(err, errlen, "cannot read the certificates of PKG_CAFILE (%s): name a file of "
                     "PEM certificates, or unset it to use the ones built in", cafile);
            mbedtls_x509_crt_free(&cas);
            return -1;
        }
        return 0;
    }
    rc = mbedtls_x509_crt_parse(&cas, pkg_ca_pem, (size_t)pkg_ca_pem_len + 1);
    if (rc < 0 || cas.version == 0) {
        snprintf(err, errlen, "this Pkg was built without certificate authorities: rebuild it, "
                 "or name a bundle with PKG_CAFILE");
        mbedtls_x509_crt_free(&cas);
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

/* The checks that failed, as one sentence: the authority first, since a
 * certificate nobody vouches for says nothing true about its name or dates. */
static void verify_words(unsigned long flags, const char *host, char *err, size_t errlen)
{
    char now[32];
    if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
        snprintf(err, errlen, "%s sends a certificate from an authority this Pkg does not know: if it is "
                 "your own authority, name its file with PKG_CAFILE", host);
    } else if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) {
        snprintf(err, errlen, "the certificate %s sends is made out to another name: name the channel with "
                 "the address the certificate carries", host);
    } else if (flags & MBEDTLS_X509_BADCERT_EXPIRED) {
        today(now, sizeof now);
        snprintf(err, errlen, "%s sends a certificate that expired: wait for the server to renew it, or, "
                 "if this machine's clock is wrong, set the date, which it believes is %s", host, now);
    } else if (flags & MBEDTLS_X509_BADCERT_FUTURE) {
        today(now, sizeof now);
        snprintf(err, errlen, "%s sends a certificate that is not valid yet: this machine's clock is "
                 "probably wrong, and it believes the date is %s", host, now);
    } else {
        snprintf(err, errlen, "the certificate %s sends cannot be checked (Mbed TLS verification "
                 "flags 0x%lx)", host, flags);
    }
}

/* ---- the session ---------------------------------------------------------- */

static int setup_once(char *err, size_t errlen)
{
    if (ready) return 0;
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    if (psa_crypto_init() != PSA_SUCCESS
        || mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                 (const unsigned char *)"pkg", 3) != 0) {
        snprintf(err, errlen, "TLS does not start on this machine: it gives no randomness "
                 "(entropy.resource, in AROS since July 2026)");
        goto fail;
    }
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        snprintf(err, errlen, "TLS does not start on this machine");
        goto fail;
    }
    if (load_cas(err, errlen) != 0)
        goto fail;
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &cas, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    ready = 1;
    return 0;
fail:
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_ssl_config_free(&conf);
    return -1;
}

int pkg_tls_open(int sock, const char *host, char *err, size_t errlen)
{
    int rc;

    pkg_tls_close();
    if (setup_once(err, errlen) != 0)
        return -1;
    mbedtls_ssl_init(&ssl);
    /* The name the server is asked for and the name the certificate must
     * carry: the same one, always. An address written as numbers is matched
     * against the addresses the certificate holds. */
    if (mbedtls_ssl_setup(&ssl, &conf) != 0 || mbedtls_ssl_set_hostname(&ssl, host) != 0) {
        snprintf(err, errlen, "out of memory starting TLS with %s", host);
        mbedtls_ssl_free(&ssl);
        return -1;
    }
    the_socket = sock;
    mbedtls_ssl_set_bio(&ssl, NULL, sock_send, sock_recv, NULL);
    active = 1;
    while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        {
            unsigned long flags = (unsigned long)mbedtls_ssl_get_verify_result(&ssl);
            if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags != 0 && flags != 0xFFFFFFFFul)
                verify_words(flags, host, err, errlen);
            else
                snprintf(err, errlen, "%s does not answer in TLS: check that the address is right, and that "
                         "nothing on the way is rewriting the connection (Mbed TLS -0x%04x)", host,
                         (unsigned)-rc);
        }
        pkg_tls_close();
        return -1;
    }
    return 0;
}

ssize_t pkg_tls_read(void *buf, size_t n)
{
    int r;
    if (!active) return -1;
    for (;;) {
        r = mbedtls_ssl_read(&ssl, (unsigned char *)buf, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
            || r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
#endif
            )
            continue;                   /* TLS 1.3 housekeeping, not data */
        break;
    }
    if (r > 0) return r;
    /* the server's own close is the end of the body, not a failure */
    return r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF ? 0 : -1;
}

/* All of it, or a failure: a record holds 16 KB at most, so one call to the
 * library may take less than it was given. */
ssize_t pkg_tls_write(const void *buf, size_t n)
{
    size_t at = 0;
    if (!active) return -1;
    while (at < n) {
        int r = mbedtls_ssl_write(&ssl, (const unsigned char *)buf + at, n - at);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r <= 0) return -1;
        at += (size_t)r;
    }
    return (ssize_t)n;
}

void pkg_tls_close(void)
{
    if (active) {
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        active = 0;
    }
}

int pkg_tls_active(void)
{
    return active;
}
