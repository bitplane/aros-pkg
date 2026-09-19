/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * What Pkg needs of Mbed TLS on AROS: a TLS 1.2 and 1.3 client that checks
 * the server's certificate. Ephemeral elliptic-curve key exchange, AES-GCM
 * and ChaCha20-Poly1305, SHA-2, RSA and ECDSA signatures. No server, no
 * DTLS, no legacy ciphers, no files, no sockets: Pkg gives it bsdsocket's
 * send and recv, the system's entropy and the date.
 */

#ifndef PKG_MBEDTLS_CONFIG_H
#define PKG_MBEDTLS_CONFIG_H

/* the system: time for certificate dates, entropy from getentropy() */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MS_TIME_ALT        /* mbedtls_ms_time() is in pkg_tls_aros.c */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT        /* mbedtls_hardware_poll() is in pkg_tls_aros.c */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* the protocol */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* TLS 1.3 works through the PSA interface; nothing is stored */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_HKDF_C

/* certificates */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* signatures and key exchange */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_NO_UDBL_DIVISION            /* no 128-bit division: the x86_64 AROS link has no __udivti3 */
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

/* hashes and ciphers */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C                      /* old certificates in a chain, never the handshake */
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

#endif
