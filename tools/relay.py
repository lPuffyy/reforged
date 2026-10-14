"""LAN relay for System Link over the Internet: a UDP hub for Ethernet frames. usage: python tools/relay.py [port=3075]
Consoles (XBOX_RELAY=host:port) send "XRL1"+frame; broadcasts go to every other console, unicast frames to the console that owns the destination MAC
(learned from source MACs). A bare "XRL1" datagram registers / keeps a client alive. Clients silent for 30 s are dropped. No game data is stored."""
import socket, sys, time
port = int(sys.argv[1]) if len(sys.argv) > 1 else 3075
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('0.0.0.0', port)); s.settimeout(5)
clients = {}; macs = {}
print('relay listening on UDP', port)
while True:
    now = time.time()
    for a in [a for a, t in clients.items() if now - t > 30]:
        del clients[a]; [macs.pop(m) for m in [m for m, x in macs.items() if x == a]]; print('drop', a)
    try: d, a = s.recvfrom(4096)
    except socket.timeout: continue
    except ConnectionResetError: continue   # Windows reports an ICMP port-unreachable (a client went away) as an error on the next recv
    if d[:4] != b'XRL1': continue
    if a not in clients: print('join', a)
    clients[a] = now
    f = d[4:]
    if len(f) < 14: continue
    macs[bytes(f[6:12])] = a
    dst = bytes(f[:6])
    targets = [a2 for a2 in clients if a2 != a] if dst[0] & 1 or dst not in macs else [macs[dst]]
    for t in targets:
        try: s.sendto(d, t)
        except OSError: pass
