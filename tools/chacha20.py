#!/usr/bin/env python3
"""A second, independent implementation of the untraceable vault's encryption.

The app's format (crypt.rs) is "LVX1" + a 12-byte nonce + the bytes XORed with ChaCha20's keystream
from that nonce, with the key = PBKDF2-HMAC-SHA256(password, b"lewd-vault/untraceable/v1", 310000).
A vault encrypted on Windows must open on iOS and back, so this is written from the spec (not shared
with the Rust code) and used to build the fixture the Rust test reads:

    python3 tools/chacha20.py build-fixture

That proves the two implementations interoperate, rather than only agreeing with themselves. The
ChaCha20 core is checked against the RFC 8439 keystream vector:

    python3 tools/chacha20.py self-test
"""
import hashlib
import os
import struct
import sys

MAGIC = b"LVX1"
SALT = b"lewd-vault/untraceable/v1"
ROUNDS = 310_000
CHECK = b"lewd vault: the password is right"


def _rotl(x, n):
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF


def _quarter_round(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] ^= s[a]; s[d] = _rotl(s[d], 16)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] ^= s[c]; s[b] = _rotl(s[b], 12)
    s[a] = (s[a] + s[b]) & 0xFFFFFFFF; s[d] ^= s[a]; s[d] = _rotl(s[d], 8)
    s[c] = (s[c] + s[d]) & 0xFFFFFFFF; s[b] ^= s[c]; s[b] = _rotl(s[b], 7)


def block(key, counter, nonce):
    state = list(struct.unpack("<4I", b"expand 32-byte k"))
    state += list(struct.unpack("<8I", key))
    state += [counter] + list(struct.unpack("<3I", nonce))
    work = state[:]
    for _ in range(10):
        _quarter_round(work, 0, 4, 8, 12); _quarter_round(work, 1, 5, 9, 13)
        _quarter_round(work, 2, 6, 10, 14); _quarter_round(work, 3, 7, 11, 15)
        _quarter_round(work, 0, 5, 10, 15); _quarter_round(work, 1, 6, 11, 12)
        _quarter_round(work, 2, 7, 8, 13); _quarter_round(work, 3, 4, 9, 14)
    return struct.pack("<16I", *[(work[i] + state[i]) & 0xFFFFFFFF for i in range(16)])


def crypt(key, nonce, data):
    out = bytearray()
    counter = 0
    while len(out) < len(data):
        out += block(key, counter, nonce)
        counter += 1
    return bytes(a ^ b for a, b in zip(data, out))


def derive(password):
    return hashlib.pbkdf2_hmac("sha256", password.encode(), SALT, ROUNDS, 32)


def encrypt(key, plaintext, nonce=None):
    nonce = os.urandom(12) if nonce is None else nonce
    return MAGIC + nonce + crypt(key, nonce, plaintext)


def self_test():
    key = bytes(range(32))
    nonce = bytes.fromhex("000000090000004a00000000")
    expect = ("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
              "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e")
    got = block(key, 1, nonce).hex()
    assert got == expect, f"RFC 8439 vector mismatch:\n{got}\n{expect}"
    print("chacha20: RFC 8439 keystream vector OK")


def build_fixture(root):
    key = derive("hunter2")
    os.makedirs(os.path.join(root, "_app"), exist_ok=True)
    # key.check: a zero nonce, so the same password makes the same bytes on every device
    with open(os.path.join(root, "_app", "key.check"), "wb") as f:
        f.write(MAGIC + bytes(12) + crypt(key, bytes(12), CHECK))
    plain = os.path.join(os.path.dirname(root), "golden-vault")
    with open(os.path.join(plain, "index.csv"), "rb") as f:
        index = f.read()
    with open(os.path.join(plain, "views.csv"), "rb") as f:
        views = f.read()
    files = {
        "index.csv": (index, bytes.fromhex("0a0b0c0d0e0f101112131415")),
        "views.csv": (views, bytes.fromhex("1a1b1c1d1e1f202122232425")),
        os.path.join("_app", "settings.json"): (b'{"theme":"berry"}', bytes.fromhex("2a2b2c2d2e2f303132333435")),
    }
    for name, (data, nonce) in files.items():
        with open(os.path.join(root, name), "wb") as f:
            f.write(encrypt(key, data, nonce))
    print(f"chacha20: wrote {root}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "self-test":
        self_test()
    elif len(sys.argv) > 1 and sys.argv[1] == "build-fixture":
        here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        build_fixture(os.path.join(here, "src-tauri", "tests", "fixtures", "golden-untraceable"))
    else:
        print(__doc__)
