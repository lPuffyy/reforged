"""Tests for the System Link relay's rules (no network needed): rooms keep traffic apart, unicast follows learned addresses, and the limits hold.
usage: python -I tests/relay_test.py"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
from relay import Relay, room_key, MAGIC2

BCAST = b'\xff' * 6
def frame(dst, src, payload=b'x' * 40): return dst + src + b'\x08\x00' + payload
def pkt(room, f=b''): return MAGIC2 + room_key(room) + f

t = [0.0]
r = Relay(max_room=3, max_rooms=2, rate=10, timeout=30.0, clock=lambda: t[0])
A1, A2, A3, A4, B1, C1 = ('1.1.1.1', 1), ('1.1.1.2', 1), ('1.1.1.3', 1), ('1.1.1.4', 1), ('2.2.2.1', 1), ('3.3.3.1', 1)
M = {k: bytes([2, 0, 0, 0, 0, i]) for i, k in enumerate((A1, A2, A3, A4, B1, C1), 1)}

# joining and broadcast stay inside the room
for a, room in ((A1, 'alpha'), (A2, 'alpha'), (B1, 'beta')): assert r.handle(pkt(room), a) == []
out = r.handle(pkt('alpha', frame(BCAST, M[A1])), A1)
assert [to for _, to in out] == [A2], out
assert r.handle(pkt('beta', frame(BCAST, M[B1])), B1) == []
print('rooms keep broadcasts apart: ok')

# unicast goes to the console that owns the destination address, and only there
r.handle(pkt('alpha', frame(BCAST, M[A2])), A2)
out = r.handle(pkt('alpha', frame(M[A2], M[A1])), A1)
assert [to for _, to in out] == [A2], out
assert out[0][0] == pkt('alpha', frame(M[A2], M[A1])), 'frames are passed on unchanged'
print('unicast follows learned addresses: ok')

# the same MAC in another room is a different console
r.handle(pkt('beta', frame(BCAST, M[A2])), B1)
assert [to for _, to in r.handle(pkt('alpha', frame(M[A2], M[A1])), A1)] == [A2]
print('rooms do not share addresses: ok')

# limits: consoles per room, rooms, frame size, rate
assert r.handle(pkt('alpha'), A3) == [] and A3 in r.clients
r.handle(pkt('alpha'), A4); assert A4 not in r.clients, 'a fourth console must not fit in a room of three'
r.handle(pkt('gamma'), C1); assert C1 not in r.clients, 'a third room must not open when two are allowed'
assert r.handle(pkt('alpha', frame(BCAST, M[A1], b'y' * 2000)), A1) == [], 'oversized frames are dropped'
n = 0
for _ in range(40): n += bool(r.handle(pkt('alpha', frame(BCAST, M[A1])), A1))
assert 0 < n <= 12, n
t[0] += 1.0
assert r.handle(pkt('alpha', frame(BCAST, M[A1])), A1), 'the rate limit refills'
print('limits hold: ok')

# silent consoles are dropped
t[0] += 31.0
assert set(r.expire()) == {A1, A2, A3, B1}
assert not r.clients and not r.macs
print('idle consoles expire: ok')

# the old room-less form still works
r.handle(b'XRL1', A1); r.handle(b'XRL1', A2)
out = r.handle(b'XRL1' + frame(BCAST, M[A1]), A1)
assert [to for _, to in out] == [A2] and out[0][0].startswith(b'XRL1'), out
print('the old XRL1 form still works: ok')
print('relay tests passed')
