"""LAN relay for System Link over the Internet: a UDP hub for Ethernet frames, with rooms.
usage: python tools/relay.py [--port 3075] [--max-room 16] [--max-rooms 256] [--rate 600]

Consoles (XBOX_RELAY=host:port, XBOX_RELAY_ROOM=<name>) send "XRL2" + a 16-byte room key + one Ethernet frame. Broadcasts go to every other console in the SAME room,
unicast frames to the console that owns the destination MAC (learned from source MACs, per room). A datagram with the header and no frame registers a console and keeps
it alive. Consoles silent for 30 s are dropped. The old "XRL1" form (no room) is still accepted and lands in a default room.

What it does not do: it does not look inside frames, store anything, or log frame contents. It logs joins and leaves (a short hash of the address, not the address).
Limits (against abuse): at most --max-room consoles per room, --max-rooms rooms, --rate frames per second per console, frames of at most 1600 bytes.
There is no encryption and no account system: anyone who knows a room name can join it, and everyone in a room can see the traffic of the others in it
(the game's own packets, which can include player names). Use a private room name and play with people you trust."""
import argparse, hashlib, socket, time

MAGIC1, MAGIC2 = b'XRL1', b'XRL2'
HEAD2 = 20   # magic + 16-byte room key


def room_key(name):
    """The key a client sends for a room name (clients compute the same function: FNV-1a 64 over the name, twice, with two different offsets)."""
    def fnv(data, offset):
        h = offset
        for b in data: h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        return h
    raw = name.encode('utf-8')
    return fnv(raw, 0xCBF29CE484222325).to_bytes(8, 'big') + fnv(raw, 0x84222325CBF29CE4).to_bytes(8, 'big')


def describe(f):
    """Headers of an Ethernet frame for debugging: type, virtual addresses, ports, size. Never the payload."""
    et = int.from_bytes(f[12:14], 'big'); d = 'bcast' if f[:6] == b'\xff' * 6 else 'uni'
    if et == 0x0806 and len(f) >= 42: return 'frame %s ARP op=%d %s -> %s (%d bytes)' % (d, int.from_bytes(f[20:22], 'big'), '.'.join(map(str, f[28:32])), '.'.join(map(str, f[38:42])), len(f))
    if et == 0x0800 and len(f) >= 34:
        proto = f[23]; src = '.'.join(map(str, f[26:30])); dst = '.'.join(map(str, f[30:34])); ports = ''
        if proto in (6, 17) and len(f) >= 38: ports = ' ports %d->%d' % (int.from_bytes(f[34:36], 'big'), int.from_bytes(f[36:38], 'big'))
        return 'frame %s IPv4 proto=%d %s -> %s%s (%d bytes)' % (d, proto, src, dst, ports, len(f))
    return 'frame %s type=0x%04x (%d bytes)' % (d, et, len(f))


class Relay:
    def __init__(self, max_room=16, max_rooms=256, rate=600, timeout=30.0, max_frame=1600, clock=time.time, trace=0):
        self.max_room, self.max_rooms, self.rate, self.timeout, self.max_frame, self.clock = max_room, max_rooms, rate, timeout, max_frame, clock
        self.clients = {}   # address -> {'room': key, 'seen': t, 'tokens': float, 'stamp': t}
        self.macs = {}      # (room key, mac) -> address
        self.dropped = 0
        self.trace = trace   # debugging: describe the headers (never contents) of this many frames
        self.counters = {}   # room key -> [keep-alives, frames in, broadcasts, unicasts, frames out]; counts only, never contents or addresses

    def _room_members(self, room): return [a for a, c in self.clients.items() if c['room'] == room]

    def _remove(self, addr):
        c = self.clients.pop(addr, None)
        if c:
            for k in [k for k, v in self.macs.items() if v == addr]: del self.macs[k]
        return c

    def summary(self):
        """One line per active room: consoles and what passed through since the last summary (a short hash stands for the room name)."""
        lines = []
        for room, cnt in sorted(self.counters.items()):
            members = len(self._room_members(room))
            if members or any(cnt): lines.append('room %s: %d console(s); keep-alives %d, frames in %d (%d broadcast, %d unicast), frames out %d' % (hashlib.sha256(room).hexdigest()[:6], members, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4]))
        self.counters = {}
        return lines

    def expire(self):
        now = self.clock(); gone = [a for a, c in self.clients.items() if now - c['seen'] > self.timeout]
        for a in gone: self._remove(a)
        return gone

    def handle(self, data, addr):
        """Processes one datagram from `addr`; returns [(bytes, address), ...] to send."""
        if data[:4] == MAGIC2 and len(data) >= HEAD2: room, frame, head = data[4:HEAD2], data[HEAD2:], HEAD2
        elif data[:4] == MAGIC1: room, frame, head = b'\0' * 16, data[4:], 4
        else: return []
        now = self.clock(); c = self.clients.get(addr)
        if c is None or c['room'] != room:
            if c is not None: self._remove(addr)
            if len(self._room_members(room)) >= self.max_room: self.dropped += 1; return []
            if not self._room_members(room) and len({x['room'] for x in self.clients.values()}) >= self.max_rooms: self.dropped += 1; return []
            c = self.clients[addr] = {'room': room, 'seen': now, 'tokens': float(self.rate), 'stamp': now}
        c['seen'] = now; cnt = self.counters.setdefault(room, [0, 0, 0, 0, 0])
        c['tokens'] = min(float(self.rate), c['tokens'] + (now - c['stamp']) * self.rate); c['stamp'] = now
        if not frame: cnt[0] += 1; return []
        if len(frame) < 14 or len(frame) > self.max_frame or c['tokens'] < 1: self.dropped += 1; return []
        c['tokens'] -= 1; cnt[1] += 1
        if self.trace > 0: self.trace -= 1; print('from %s: %s' % (hashlib.sha256(repr(addr).encode()).hexdigest()[:6], describe(frame)), flush=True)
        self.macs[(room, bytes(frame[6:12]))] = addr
        dst = bytes(frame[:6]); owner = self.macs.get((room, dst))
        targets = [a for a in self._room_members(room) if a != addr] if (dst[0] & 1 or owner is None) else ([owner] if owner != addr else [])
        cnt[2 if (dst[0] & 1 or owner is None) else 3] += 1; cnt[4] += len(targets)
        out = MAGIC2 + room + frame if head == HEAD2 else MAGIC1 + frame
        return [(out, t) for t in targets]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0]); ap.add_argument('--port', type=int, default=3075); ap.add_argument('--max-room', type=int, default=16)
    ap.add_argument('--trace', type=int, default=0, help='debugging: describe the headers of the first N frames');
    ap.add_argument('--max-rooms', type=int, default=256); ap.add_argument('--rate', type=int, default=600, help='frames per second per console'); a = ap.parse_args()
    relay = Relay(a.max_room, a.max_rooms, a.rate, trace=a.trace)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('0.0.0.0', a.port)); s.settimeout(5)
    print('relay listening on UDP', a.port, flush=True)
    known = set(); last_stats = time.time()
    while True:
        for g in relay.expire(): print('drop', hashlib.sha256(repr(g).encode()).hexdigest()[:8], flush=True); known.discard(g)
        if time.time() - last_stats > 60:
            last_stats = time.time()
            for line in relay.summary(): print(line, flush=True)
        try: d, addr = s.recvfrom(4096)
        except socket.timeout: continue
        except ConnectionResetError: continue   # Windows reports an ICMP port-unreachable (a client went away) as an error on the next recv
        for pkt, to in relay.handle(d, addr):
            try: s.sendto(pkt, to)
            except OSError: pass
        if addr in relay.clients and addr not in known: known.add(addr); print('join', hashlib.sha256(repr(addr).encode()).hexdigest()[:8], '(%d consoles)' % len(relay.clients), flush=True)


if __name__ == '__main__': main()
