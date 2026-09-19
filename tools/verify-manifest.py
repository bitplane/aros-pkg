#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Checks a package's signature without Pkg: a second opinion, in one file.

    verify-manifest.py <digest>.manifest <digest>.sig [expected signer]

Says whether the manifest's SHA-256 is the digest in its name, whether the
Ed25519 signature in the .sig file is valid for the manifest bytes under the
public key the .sig names, and, when a signer is given, whether it is that
key. Exit 0 when everything holds, 1 otherwise. Plain Python 3, no modules
beyond the standard library: the Ed25519 check follows RFC 8032 directly, so
it is slow (a second) and easy to read.
"""
import hashlib, os, sys

q = 2**255 - 19
l = 2**252 + 27742317777372353535851937790883648493
def inv(x): return pow(x, q - 2, q)
d = -121665 * inv(121666) % q
I = pow(2, (q - 1) // 4, q)

def xrecover(y):
    xx = (y * y - 1) * inv(d * y * y + 1) % q
    x = pow(xx, (q + 3) // 8, q)
    if (x * x - xx) % q: x = x * I % q
    if x % 2: x = q - x
    return x

B = (xrecover(4 * inv(5) % q), 4 * inv(5) % q)

def add(P, Q):
    (x1, y1), (x2, y2) = P, Q
    x3 = (x1 * y2 + x2 * y1) * inv(1 + d * x1 * x2 * y1 * y2)
    y3 = (y1 * y2 + x1 * x2) * inv(1 - d * x1 * x2 * y1 * y2)
    return (x3 % q, y3 % q)

def mul(P, e):
    R = (0, 1)
    while e:
        if e & 1: R = add(R, P)
        P = add(P, P); e >>= 1
    return R

def decode(s):
    y = int.from_bytes(s, "little") & ((1 << 255) - 1)
    x = xrecover(y)
    if x & 1 != s[31] >> 7: x = q - x
    return (x, y)

def encode(P):
    x, y = P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")

def verify(pk, sig, msg):
    if len(pk) != 32 or len(sig) != 64: return False
    A, R = decode(pk), decode(sig[:32])
    S = int.from_bytes(sig[32:], "little")
    if S >= l: return False
    h = int.from_bytes(hashlib.sha512(sig[:32] + pk + msg).digest(), "little") % l
    return encode(mul(B, S)) == encode(add(R, mul(A, h)))

def main(argv):
    if len(argv) < 3:
        print(__doc__.strip().split("\n")[2]); return 2
    manifest, sigfile = argv[1], argv[2]
    expected = argv[3].lower() if len(argv) > 3 else None
    m = open(manifest, "rb").read()
    fields = dict(line.split(": ", 1) for line in open(sigfile).read().split("\n") if ": " in line)
    signer, signature = fields.get("Signer", "").strip(), fields.get("Signature", "").strip()
    ok = True
    digest = hashlib.sha256(m).hexdigest()
    named = os.path.basename(manifest).split(".")[0]
    if digest == named:
        print("digest:    the manifest is the one its name says, sha256 %s" % digest[:16])
    else:
        print("digest:    MISMATCH, the file's sha256 is %s, its name says %s" % (digest[:16], named[:16])); ok = False
    if verify(bytes.fromhex(signer), bytes.fromhex(signature), m):
        print("signature: valid, made by %s" % signer)
    else:
        print("signature: INVALID under the key the .sig names"); ok = False
    if expected:
        if signer == expected: print("signer:    the expected key")
        else: print("signer:    NOT the expected key %s" % expected); ok = False
    print("result:    %s" % ("the manifest is what its publisher signed" if ok else "do not trust this package"))
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main(sys.argv))
