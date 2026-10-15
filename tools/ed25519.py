"""Ed25519 signature verification in pure Python (RFC 8032), so the companion tool needs no third-party package to check a downloaded release.
Verification only. Slow (tens of milliseconds) but it runs once per download.
    import ed25519; ed25519.verify(public_key_32_bytes, message, signature_64_bytes) -> bool"""
import hashlib

P = 2 ** 255 - 19
Q = 2 ** 252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
I = pow(2, (P - 1) // 4, P)


def _inv(x): return pow(x, P - 2, P)


def _xrecover(y):
    xx = (y * y - 1) * _inv(D * y * y + 1) % P
    x = pow(xx, (P + 3) // 8, P)
    if (x * x - xx) % P != 0: x = x * I % P
    if x % 2 != 0: x = P - x
    return x


_BY = 4 * _inv(5) % P
_BX = _xrecover(_BY)
B = (_BX % P, _BY % P, 1, _BX * _BY % P)
IDENT = (0, 1, 1, 0)


def _add(p1, p2):
    x1, y1, z1, t1 = p1; x2, y2, z2, t2 = p2
    a = (y1 - x1) * (y2 - x2) % P; b = (y1 + x1) * (y2 + x2) % P
    c = t1 * 2 * D * t2 % P; d = z1 * 2 * z2 % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(pt, e):
    r = IDENT
    while e:
        if e & 1: r = _add(r, pt)
        pt = _add(pt, pt); e >>= 1
    return r


def _encode(pt):
    x, y, z, _ = pt; zi = _inv(z); x = x * zi % P; y = y * zi % P
    return (y | ((x & 1) << 255)).to_bytes(32, 'little')


def _decode(s):
    if len(s) != 32: return None
    y = int.from_bytes(s, 'little') & ((1 << 255) - 1)
    if y >= P: return None
    x = _xrecover(y)
    if (x * x - (y * y - 1) * _inv(D * y * y + 1)) % P != 0: return None
    if (x & 1) != (s[31] >> 7): x = P - x
    return (x, y, 1, x * y % P)


def verify(public_key, message, signature):
    if len(public_key) != 32 or len(signature) != 64: return False
    r = _decode(signature[:32]); a = _decode(public_key)
    if r is None or a is None: return False
    s = int.from_bytes(signature[32:], 'little')
    if s >= Q: return False
    h = int.from_bytes(hashlib.sha512(signature[:32] + public_key + message).digest(), 'little') % Q
    return _encode(_mul(B, s)) == _encode(_add(r, _mul(a, h)))


def _self_test():
    # RFC 8032 section 7.1, test 1 (empty message)
    pub = bytes.fromhex('d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')
    sig = bytes.fromhex('e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b')
    assert verify(pub, b'', sig)
    assert not verify(pub, b'x', sig)
    bad = bytearray(sig); bad[10] ^= 1
    assert not verify(pub, b'', bytes(bad))
    print('ed25519 self test passed')


if __name__ == '__main__': _self_test()
