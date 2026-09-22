/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 John Knipper
 *
 * Signing keys: reading, writing, pinning, and OpenSSH's formats for
 * machines that have ssh-keygen and no pkg.
 *
 * Part of libpkg: see pkg_internal.h for how the library is split.
 */

#include "pkg_internal.h"

/* ---- keys ------------------------------------------------------------- */

int cmd_keygen(const struct pkg_options *a)
{
    unsigned char seed[PKG_ED25519_SEED];
    struct key k;
    char seedhex[2 * PKG_ED25519_SEED + 1], text[256];
    int n;

    if (a->file == NULL)
        return refuse_c(20, "name the key file with FILE <path>");
    if (pkg_fs_exists(a->file))
        return refuse_c(15, "\"%s\" already exists; a key is never overwritten", a->file);
    if (pkg_fs_random(seed, sizeof seed) != 0) {
        int typed = machine ? -2 : pkg_fs_random_typed(seed, sizeof seed);
        if (typed == -2)
            return refuse_c(17, "this system has no random source, and a key made without one could be "
                            "guessed: run KEYGEN in a Shell window, where pkg makes it from the moments "
                            "you press keys, or make the key on a Mac or a PC and bring the file here");
        if (typed != 0)
            return refuse_c(17, "the key was not made: typing stopped before there was enough of it");
    }
    pkg_ed25519_keypair(k.pk, k.sk, seed);
    tohex(seed, sizeof seed, seedhex);
    tohex(k.pk, sizeof k.pk, k.pkhex);
    n = snprintf(text, sizeof text, "Pkg-Secret-Key: 1\nSeed: %s\nPublic: %s\n",
                 seedhex, k.pkhex);
    if (pkg_fs_write_private(a->file, text, (size_t)n) != 0)
        return refuse_c(17, "cannot write \"%s\": %s", a->file, strerror(errno));
    memset(seed, 0, sizeof seed);
    memset(seedhex, 0, sizeof seedhex);
    memset(text, 0, sizeof text);
    kv("result", "created");
    kv("file", "%s", a->file);
    kv("public", "%s", k.pkhex);
    if (!machine) {
        say_result("key written to %s, readable by you alone", a->file);
        say_detail("public key %s", k.pkhex);
    }
    hint("every later version of what this key publishes must be signed with it: keep the "
         "file with the person's secrets, outside any channel or repository, and back it up. "
         "Use it with SIGN <file> or PKG_SIGNKEY; only the public key may be shared");
    return 0;
}

int load_key(const char *path, struct key *k)
{
    unsigned char *buf, seed[PKG_ED25519_SEED], pk[PKG_ED25519_PUBLIC];
    size_t len;
    char seedhex[65], pubhex[65];

    if (path == NULL)
        return refuse_c(14, "no signing key: give SIGN <keyfile>, or set PKG_SIGNKEY. "
                        "A publisher who has published before must sign with the same key, or "
                        "every machine that installed their packages refuses the new ones: ask "
                        "whoever requested this where theirs is before creating one with KEYGEN. "
                        "DRYRUN needs no key");
    if (pkg_fs_read(path, &buf, &len) != 0)
        return refuse_c(17, "cannot read the key \"%s\": %s", path, strerror(errno));
    if (len > 512u
        || sscanf((const char *)buf, "Pkg-Secret-Key: 1\nSeed: %64s\nPublic: %64s",
                  seedhex, pubhex) != 2
        || fromhex(seed, sizeof seed, seedhex) != 0
        || fromhex(pk, sizeof pk, pubhex) != 0) {
        free(buf);
        return refuse_c(12, "\"%s\" is not a pkg key file", path);
    }
    memset(buf, 0, len);
    free(buf);
    pkg_ed25519_keypair(k->pk, k->sk, seed);
    memset(seed, 0, sizeof seed);
    if (memcmp(k->pk, pk, sizeof pk) != 0)
        return refuse_c(12, "\"%s\" is damaged: its public key does not match its seed", path);
    tohex(k->pk, sizeof k->pk, k->pkhex);
    return 0;
}

int write_sig(const char *path, const struct key *k,
              const unsigned char *msg, size_t len)
{
    unsigned char sig[PKG_ED25519_SIG];
    char sighex[2 * PKG_ED25519_SIG + 1], text[256];
    int n;
    pkg_ed25519_sign(sig, msg, len, k->sk);
    tohex(sig, sizeof sig, sighex);
    n = snprintf(text, sizeof text, "Signer: %s\nSignature: %s\n", k->pkhex, sighex);
    return pkg_fs_write_atomic(path, text, (size_t)n);
}

/* Verify a detached signature file over msg. On success the signer's public
 * key is copied to signer (65 bytes). */
int check_sig(const char *sigpath, const unsigned char *msg, size_t len,
              char signer[65], const char *what)
{
    unsigned char *buf, pk[PKG_ED25519_PUBLIC], sig[PKG_ED25519_SIG];
    size_t blen;
    char pkhex[65], sighex[129];

    if (pkg_fs_read(sigpath, &buf, &blen) != 0)
        return refuse_c(13, "%s is not signed. Every package is signed; nothing was installed", what);
    if (blen > 512u
        || sscanf((const char *)buf, "Signer: %64s\nSignature: %128s", pkhex, sighex) != 2
        || fromhex(pk, sizeof pk, pkhex) != 0 || fromhex(sig, sizeof sig, sighex) != 0) {
        free(buf);
        return refuse_c(13, "the signature of %s is malformed; nothing was installed", what);
    }
    free(buf);
    if (pkg_ed25519_verify(sig, msg, len, pk) != 0)
        return refuse_c(13, "the signature of %s does not verify against its manifest, so the "
                      "manifest or the signature was altered after signing; nothing was installed",
                      what);
    memcpy(signer, pkhex, 65);
    return 0;
}

/* ---- OpenSSH's formats, for machines that have ssh-keygen and no Pkg ---- *
 * The same Ed25519 key, written as OpenSSH writes it: the public key as an
 * "ssh-ed25519" line, and a signature in the SSHSIG format (PROTOCOL.sshsig
 * in OpenSSH's sources) that `ssh-keygen -Y verify` checks. */

struct sshbuf { unsigned char b[512]; size_t n; };

static void ssh_put(struct sshbuf *s, const void *p, size_t len)   /* a string: length, bytes */
{
    s->b[s->n++] = (unsigned char)(len >> 24);
    s->b[s->n++] = (unsigned char)(len >> 16);
    s->b[s->n++] = (unsigned char)(len >> 8);
    s->b[s->n++] = (unsigned char)len;
    memcpy(s->b + s->n, p, len);
    s->n += len;
}

static void ssh_put_str(struct sshbuf *s, const char *str) { ssh_put(s, str, strlen(str)); }

static void ssh_pubkey_blob(struct sshbuf *s, const unsigned char pk[PKG_ED25519_PUBLIC])
{
    s->n = 0;
    ssh_put_str(s, "ssh-ed25519");
    ssh_put(s, pk, PKG_ED25519_PUBLIC);
}

/* Base64 of len bytes into out, a line break every wrap characters when wrap
 * is not 0; out needs 4 * len / 3 + len / wrap + 8 bytes. */
static void base64(const unsigned char *in, size_t len, char *out, size_t wrap)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, col = 0;
    for (i = 0; i < len; i += 3) {
        unsigned long v = (unsigned long)in[i] << 16
                        | (i + 1 < len ? (unsigned long)in[i + 1] << 8 : 0)
                        | (i + 2 < len ? in[i + 2] : 0);
        char q[4];
        int k;
        q[0] = t[v >> 18 & 63];
        q[1] = t[v >> 12 & 63];
        q[2] = i + 1 < len ? t[v >> 6 & 63] : '=';
        q[3] = i + 2 < len ? t[v & 63] : '=';
        for (k = 0; k < 4; k++) {
            if (wrap && col == wrap) { *out++ = '\n'; col = 0; }
            *out++ = q[k];
            col++;
        }
    }
    *out = '\0';
}

/* "ssh-ed25519 AAAA... <comment>", the comment the key file's name without
 * its directory and extension: jkn for ~/.config/aros-pkg/jkn.key. */
static void ssh_pubkey_line(const struct key *k, const char *path, char *out, size_t outsz)
{
    struct sshbuf s;
    char b64[128], comment[64];
    const char *base = path, *p;
    size_t cl;
    for (p = path; *p; p++)
        if (*p == '/' || *p == ':')
            base = p + 1;
    p = strrchr(base, '.');
    cl = p != NULL && p != base ? (size_t)(p - base) : strlen(base);
    if (cl >= sizeof comment) cl = sizeof comment - 1;
    memcpy(comment, base, cl);
    comment[cl] = '\0';
    for (p = comment; *p; p++)
        if (*p == ' ' || (unsigned char)*p < 0x21)
            comment[p - comment] = '-';
    ssh_pubkey_blob(&s, k->pk);
    base64(s.b, s.n, b64, 0);
    if (snprintf(out, outsz, "ssh-ed25519 %s %s", b64, comment) >= (int)outsz)
        out[outsz - 1] = '\0';   /* a long comment is cut; the key is whole */
}

/* An SSHSIG signature over msg for namespace ns, armored, as ssh-keygen -Y
 * sign writes it: Ed25519 over "SSHSIG", the namespace, an empty reserved
 * string, "sha512" and the SHA-512 of the message. */
static int ssh_sign(const char *path, const struct key *k, const char *ns,
                    const unsigned char *msg, size_t len)
{
    struct sshbuf tosign, out, pub, sigb;
    unsigned char h[PKG_SHA512_LEN], sig[PKG_ED25519_SIG];
    char text[1024], b64[700];
    int n;

    pkg_sha512(h, msg, len);
    tosign.n = 0;
    memcpy(tosign.b, "SSHSIG", 6);
    tosign.n = 6;
    ssh_put_str(&tosign, ns);
    ssh_put_str(&tosign, "");
    ssh_put_str(&tosign, "sha512");
    ssh_put(&tosign, h, sizeof h);
    pkg_ed25519_sign(sig, tosign.b, tosign.n, k->sk);

    ssh_pubkey_blob(&pub, k->pk);
    sigb.n = 0;
    ssh_put_str(&sigb, "ssh-ed25519");
    ssh_put(&sigb, sig, sizeof sig);
    memcpy(out.b, "SSHSIG", 6);
    out.n = 6;
    out.b[out.n++] = 0; out.b[out.n++] = 0; out.b[out.n++] = 0; out.b[out.n++] = 1;
    ssh_put(&out, pub.b, pub.n);
    ssh_put_str(&out, ns);
    ssh_put_str(&out, "");
    ssh_put_str(&out, "sha512");
    ssh_put(&out, sigb.b, sigb.n);
    base64(out.b, out.n, b64, 70);
    n = snprintf(text, sizeof text,
                 "-----BEGIN SSH SIGNATURE-----\n%s\n-----END SSH SIGNATURE-----\n", b64);
    return pkg_fs_write_atomic(path, text, (size_t)n);
}

/* Which public key a key file holds, without showing its secret. */
int cmd_keyinfo(const struct pkg_options *a)
{
    struct key k;
    char line[200];
    const char *path = a->file ? a->file : a->target ? a->target : a->sign;
    if (path == NULL)
        return refuse_c(20, "name the key file with FILE <keyfile>");
    if (load_key(path, &k) != 0)
        return 1;
    kv("result", "shown");
    kv("file", "%s", path);
    kv("public", "%s", k.pkhex);
    if (a->ssh) {
        ssh_pubkey_line(&k, path, line, sizeof line);
        kv("ssh", "%s", line);
        if (!machine)
            printf("%s\n", line);     /* alone on its line, for an allowed_signers file */
    } else if (!machine)
        say_result("%s holds the public key %s", path, k.pkhex);
    memset(&k, 0, sizeof k);
    return 0;
}

int cmd_sign(const struct pkg_options *a)
{
    struct key k;
    unsigned char *buf;
    size_t len;
    if (a->target == NULL || a->key == NULL || a->out == NULL)
        return refuse_c(20, "usage: SIGN <file> KEY <keyfile> OUT <sigfile> [SSH NAMESPACE <ns>]");
    if (a->ssh && (a->nspace == NULL || a->nspace[0] == '\0' || strlen(a->nspace) > 64))
        return refuse_c(20, "SSH signs for a namespace, the word the verifier names with -n: "
                        "give NAMESPACE <ns>, at most 64 characters");
    if (!a->ssh && a->nspace != NULL)
        return refuse_c(20, "NAMESPACE belongs to an SSH signature: add SSH, or leave NAMESPACE out");
    if (load_key(a->key, &k) != 0)
        return 1;
    if (pkg_fs_read(a->target, &buf, &len) != 0)
        return refuse_c(17, "cannot read \"%s\"", a->target);
    if ((a->ssh ? ssh_sign(a->out, &k, a->nspace, buf, len)
                : write_sig(a->out, &k, buf, len)) != 0) {
        free(buf);
        return refuse_c(17, "cannot write \"%s\"", a->out);
    }
    free(buf);
    kv("result", "signed");
    kv("file", "%s", a->target);
    kv("signer", "%s", k.pkhex);
    if (!machine)
        say_result("signed %s with %.16s", a->target, k.pkhex);
    return 0;
}

/* CHECKSIG <file> FILE <sigfile> [KEY <public key>]: the counterpart of SIGN
 * for a machine without ssh-keygen, and what a portal runs to check a signed
 * push request. */
int cmd_checksig(const struct pkg_options *a)
{
    unsigned char *buf;
    size_t len;
    char signer[65];
    if (a->target == NULL || a->file == NULL)
        return refuse_c(20, "usage: CHECKSIG <file> FILE <sigfile> [KEY <the signer's public key>]");
    if (pkg_fs_read(a->target, &buf, &len) != 0)
        return refuse_c(17, "cannot read \"%s\"", a->target);
    {
        unsigned char *sb, pk[PKG_ED25519_PUBLIC], sig[PKG_ED25519_SIG];
        size_t sl;
        char sighex[129];
        if (pkg_fs_read(a->file, &sb, &sl) != 0) {
            free(buf);
            return refuse_c(17, "cannot read the signature \"%s\"", a->file);
        }
        if (sl > 512u || sscanf((const char *)sb, "Signer: %64s\nSignature: %128s", signer, sighex) != 2
            || fromhex(pk, sizeof pk, signer) != 0 || fromhex(sig, sizeof sig, sighex) != 0) {
            free(sb); free(buf);
            return refuse_c(13, "\"%s\" is not a signature SIGN wrote: a Signer: line and a Signature: line", a->file);
        }
        free(sb);
        if (pkg_ed25519_verify(sig, buf, len, pk) != 0) {
            free(buf);
            return refuse_c(13, "the signature does not check: %s or %s was changed after signing, "
                            "or the signature is for another file", a->target, a->file);
        }
    }
    free(buf);
    if (a->key != NULL && ascii_casecmp(a->key, signer) != 0)
        return refuse_c(14, "%s is signed, by %s and not by the key given, %s", a->target, signer, a->key);
    kv("result", "good");
    kv("file", "%s", a->target);
    kv("signer", "%s", signer);
    if (!machine)
        say_result("%s is signed by %s%s", a->target, signer, a->key ? ", the key given" : "");
    return 0;
}
