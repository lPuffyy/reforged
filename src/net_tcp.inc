// Included by net.cpp inside its anonymous namespace (uses its helpers: SendIp, Csum, Wr16/Wr32, Rd16/Rd32, NL, SendUdp, kGuestIp).

// ---- TCP NAT: the guest's TCP connection is terminated here and relayed over a host socket (one thread per flow) ----
struct TcpFlow {
  SOCKET s = INVALID_SOCKET; uint16_t gport = 0, rport = 0; uint32_t rip = 0;
  uint32_t gnext = 0 /* next guest sequence number we expect */, snd_una = 0, snd_nxt = 0, gwnd = 8192;
  bool established = false, gfin = false, sentFin = false, dead = false;
  struct Seg { uint32_t seq; std::vector<uint8_t> data; bool fin; uint32_t t; };
  std::vector<Seg> unacked; std::mutex mx;
};
std::map<uint64_t, TcpFlow*> g_tcp; std::mutex g_tcpMx;

void SendTcp(TcpFlow* f, uint32_t seq, uint8_t flags, const uint8_t* data, uint32_t len, bool mss = false) {
  uint32_t hl = mss ? 24 : 20; std::vector<uint8_t> t(hl + len);
  Wr16(&t[0], f->rport); Wr16(&t[2], f->gport); Wr32(&t[4], seq); Wr32(&t[8], (flags & 0x10) ? f->gnext : 0); t[12] = (uint8_t)((hl / 4) << 4); t[13] = flags; Wr16(&t[14], 32768);
  if (mss) { t[20] = 2; t[21] = 4; Wr16(&t[22], 1400); }
  if (len) memcpy(&t[hl], data, len);
  uint8_t ph[12]; Wr32(ph, f->rip); Wr32(ph + 4, kGuestIp); ph[8] = 0; ph[9] = 6; Wr16(ph + 10, (uint16_t)t.size());
  uint32_t sum = 0; for (int i = 0; i < 12; i += 2) sum += ph[i] << 8 | ph[i + 1];
  Wr16(&t[16], 0); Wr16(&t[16], Csum(t.data(), t.size(), sum));
  SendIp(f->rip, kGuestIp, 6, t.data(), (uint32_t)t.size());
}

uint32_t TcpThread(void* p) {
  TcpFlow* f = (TcpFlow*)p;
  sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(f->rport); a.sin_addr.s_addr = htonl(f->rip);
  f->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  NetSetNonBlocking(f->s, true); connect(f->s, (sockaddr*)&a, sizeof a);
  fd_set ws, es; FD_ZERO(&ws); FD_ZERO(&es); FD_SET(f->s, &ws); FD_SET(f->s, &es); timeval tv = {6, 0};
  int r = select((int)f->s + 1, nullptr, &ws, &es, &tv); int err = 0; socklen_t el = sizeof err; getsockopt(f->s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
  if (r <= 0 || err || FD_ISSET(f->s, &es)) {
    NL("TCP connect to %u.%u.%u.%u:%u failed", f->rip >> 24, (f->rip >> 16) & 255, (f->rip >> 8) & 255, f->rip & 255, f->rport);
    std::lock_guard<std::mutex> l(f->mx); f->snd_nxt = f->snd_una = (uint32_t)plat::TickMs(); SendTcp(f, f->snd_nxt, 0x14, nullptr, 0); f->dead = true;   // RST|ACK
  } else {
    NetSetNonBlocking(f->s, false);
    { std::lock_guard<std::mutex> l(f->mx); f->snd_una = f->snd_nxt = (uint32_t)plat::TickMs() * 977u; SendTcp(f, f->snd_nxt, 0x12, nullptr, 0, true); f->snd_nxt++; }
    NL("TCP connected to %u.%u.%u.%u:%u", f->rip >> 24, (f->rip >> 16) & 255, (f->rip >> 8) & 255, f->rip & 255, f->rport);
    uint8_t buf[1400];
    while (!f->dead) {
      { std::lock_guard<std::mutex> l(f->mx);   // retransmit anything unacknowledged for more than 400 ms (the guest NIC can drop frames when its RX ring is full)
        for (auto& sg : f->unacked) if ((uint32_t)plat::TickMs() - sg.t > 400) { SendTcp(f, sg.seq, sg.fin ? 0x11 : 0x18, sg.data.data(), (uint32_t)sg.data.size()); sg.t = (uint32_t)plat::TickMs(); } }
      bool canSend; { std::lock_guard<std::mutex> l(f->mx); uint32_t infl = f->snd_nxt - f->snd_una; canSend = f->established && !f->sentFin && infl < (f->gwnd < 24000 ? f->gwnd : 24000); }
      int sr = 0; if (canSend) { fd_set rs; FD_ZERO(&rs); FD_SET(f->s, &rs); tv = {0, 30000}; sr = select((int)f->s + 1, &rs, nullptr, nullptr, &tv); } else plat::SleepMs(10);
      if (sr > 0) {
        int n = recv(f->s, (char*)buf, sizeof buf, 0); std::lock_guard<std::mutex> l(f->mx);
        if (n > 0) { SendTcp(f, f->snd_nxt, 0x18, buf, n); f->unacked.push_back({f->snd_nxt, std::vector<uint8_t>(buf, buf + n), false, (uint32_t)plat::TickMs()}); f->snd_nxt += n; }
        else { SendTcp(f, f->snd_nxt, 0x11, nullptr, 0); f->unacked.push_back({f->snd_nxt, {}, true, (uint32_t)plat::TickMs()}); f->snd_nxt++; f->sentFin = true; }
      }
      { std::lock_guard<std::mutex> l(f->mx); if (f->sentFin && f->gfin && f->unacked.empty()) break; }
    }
  }
  if (f->s != INVALID_SOCKET) closesocket(f->s);
  std::lock_guard<std::mutex> l(g_tcpMx); g_tcp.erase((uint64_t)f->gport << 48 | (uint64_t)f->rip << 16 | f->rport); delete f; return 0;
}

void HandleTcp(uint32_t dst, const uint8_t* t, uint32_t len) {
  if (len < 20) return;
  uint16_t gp = Rd16(t), rp = Rd16(t + 2); uint32_t seq = Rd32(t + 4), ack = Rd32(t + 8); uint8_t off = (t[12] >> 4) * 4, fl = t[13]; uint16_t wnd = Rd16(t + 14);
  if (off > len) return; const uint8_t* d = t + off; uint32_t dl = len - off;
  uint64_t key = (uint64_t)gp << 48 | (uint64_t)dst << 16 | rp; TcpFlow* f = nullptr;
  { std::lock_guard<std::mutex> l(g_tcpMx); auto it = g_tcp.find(key); if (it != g_tcp.end()) f = it->second; }
  if (!f) {
    if ((fl & 0x02) && !(fl & 0x10)) {
      f = new TcpFlow; f->gport = gp; f->rport = rp; f->rip = dst; f->gnext = seq + 1; f->gwnd = wnd;
      { std::lock_guard<std::mutex> l(g_tcpMx); g_tcp[key] = f; }
      NL("TCP SYN guest:%u -> %u.%u.%u.%u:%u", gp, dst >> 24, (dst >> 16) & 255, (dst >> 8) & 255, dst & 255, rp);
      plat::ThreadSpawn(TcpThread, f);
    }
    return;   // anything else for an unknown flow is ignored (the guest retries or times out)
  }
  std::lock_guard<std::mutex> l(f->mx); f->gwnd = wnd ? wnd : 1;
  if (fl & 0x04) { f->dead = true; return; }                                                             // RST
  if (fl & 0x02) { SendTcp(f, f->snd_una, 0x12, nullptr, 0, true); return; }                              // duplicate SYN: repeat the SYN-ACK
  if (fl & 0x10) {                                                                                        // ACK: free acknowledged segments
    f->established = true; if ((int32_t)(ack - f->snd_una) > 0 && (int32_t)(ack - f->snd_nxt) <= 0) f->snd_una = ack;
    for (size_t i = 0; i < f->unacked.size();) { uint32_t end = f->unacked[i].seq + (uint32_t)f->unacked[i].data.size() + (f->unacked[i].fin ? 1 : 0); if ((int32_t)(ack - end) >= 0) f->unacked.erase(f->unacked.begin() + i); else ++i; }
  }
  if (dl) {   // in-order data is relayed to the host socket; out-of-order or duplicate data just gets an ACK for what we expect
    if (seq == f->gnext && f->s != INVALID_SOCKET) { send(f->s, (const char*)d, (int)dl, 0); f->gnext += dl; }
    SendTcp(f, f->snd_nxt, 0x10, nullptr, 0);
  }
  if ((fl & 0x01) && seq + dl == f->gnext) { f->gnext++; f->gfin = true; if (f->s != INVALID_SOCKET) shutdown(f->s, SHUT_WR); SendTcp(f, f->snd_nxt, 0x10, nullptr, 0); }
}

// ---- UDP broadcast (System Link): forward the guest's broadcasts to the host LAN and bring LAN broadcasts back ----
std::map<uint16_t, SOCKET> g_listen; std::mutex g_listenMx;
uint32_t ListenThread(void* p) {
  SOCKET s = (SOCKET)(uintptr_t)p; uint8_t buf[2048];
  for (;;) {
    sockaddr_in from; socklen_t fl = sizeof from; int n = recvfrom(s, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl); if (n <= 0) { plat::SleepMs(50); continue; }
    sockaddr_in me; socklen_t ml = sizeof me; getsockname(s, (sockaddr*)&me, &ml); uint32_t src = ntohl(from.sin_addr.s_addr); if (src == 0x7F000001) continue;
    static uint8_t bc[6] = {255, 255, 255, 255, 255, 255};
    NL("LAN broadcast %d bytes from %u.%u.%u.%u:%u -> guest", n, src >> 24, (src >> 16) & 255, (src >> 8) & 255, src & 255, ntohs(from.sin_port));
    SendUdp(src, ntohs(from.sin_port), 0xFFFFFFFF, ntohs(me.sin_port), buf, (uint32_t)n, bc);
  }
}
void HandleBroadcast(uint16_t sp, uint16_t dp, const uint8_t* d, uint32_t len) {
  SOCKET s = INVALID_SOCKET;
  { std::lock_guard<std::mutex> l(g_listenMx);
    auto it = g_listen.find(dp);
    if (it == g_listen.end()) {
      SOCKET ns = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); int on = 1; setsockopt(ns, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof on); setsockopt(ns, SOL_SOCKET, SO_BROADCAST, (char*)&on, sizeof on);
      sockaddr_in b = {}; b.sin_family = AF_INET; b.sin_port = htons(dp);
      if (bind(ns, (sockaddr*)&b, sizeof b) == 0) { g_listen[dp] = ns; plat::ThreadSpawn(ListenThread, (void*)(uintptr_t)ns); s = ns; } else closesocket(ns);
    } else s = it->second; }
  if (s == INVALID_SOCKET) return;
  sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(dp); a.sin_addr.s_addr = INADDR_BROADCAST; sendto(s, (const char*)d, (int)len, 0, (sockaddr*)&a, sizeof a);
  NL("UDP broadcast guest:%u -> LAN:%u (%u bytes)", sp, dp, len);
}
