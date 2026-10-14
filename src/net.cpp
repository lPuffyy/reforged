// Network backend: a tiny user-mode router/NAT between the emulated nForce NIC and the host network (no drivers needed). The guest believes it is on 10.0.2.0/24:
//   10.0.2.2  gateway (ARP, ICMP echo, DHCP server)     10.0.2.3  DNS proxy -> upstream resolver     10.0.2.15  the Xbox (leased by our DHCP)
// Implemented: ARP, DHCP, ICMP echo (gateway), DNS forwarding, UDP NAT (per-flow host sockets), UDP broadcast pass-through is not done yet. TCP is not implemented.
// Debug: XBOX_NET_PCAP=path writes every frame in both directions to a pcap file; XBOX_NET_LOG=1 logs frames and flows; XBOX_DNS=a.b.c.d picks the upstream DNS server.
#include "kernel.h"
#include "net_sock.h"
#include "plat.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

void NvnetReceive(const uint8_t* frame, uint32_t len);
uint32_t NvnetMac(uint8_t* mac);

namespace {
const uint8_t kGwMac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
const uint32_t kGwIp = 0x0A000202, kDnsIp = 0x0A000203, kMask = 0xFFFFFF00;   // host-order
uint32_t kGuestIp = 0x0A00020F;   // XBOX_GUEST_IP overrides it (every console on a relay LAN needs its own address)
bool g_log = getenv("XBOX_NET_LOG") != nullptr; int g_logN = 200;
FILE* g_pcap = nullptr; std::mutex g_pcapMx;
bool g_wsa = false;
uint8_t g_guestMac[6] = {0x00, 0x50, 0xF2, 0x01, 0x02, 0x03}; uint32_t g_guestIpSeen = 0;

uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t Rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void Wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void Wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
uint16_t Csum(const uint8_t* d, size_t n, uint32_t init = 0) { uint32_t s = init; for (size_t i = 0; i + 1 < n; i += 2) s += d[i] << 8 | d[i + 1]; if (n & 1) s += d[n - 1] << 8; while (s >> 16) s = (s & 0xFFFF) + (s >> 16); return (uint16_t)~s; }
void NL(const char* f, ...) { if (!g_log || g_logN <= 0) return; --g_logN; char b[300]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); Log("[net] %s", b); }

void Pcap(const uint8_t* f, uint32_t len) {
  if (!g_pcap) return; std::lock_guard<std::mutex> l(g_pcapMx); uint32_t h[4]; SYSTEMTIME st; GetSystemTime(&st); FILETIME ft; SystemTimeToFileTime(&st, &ft); uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10 - 11644473600000000ull;
  h[0] = (uint32_t)(t / 1000000); h[1] = (uint32_t)(t % 1000000); h[2] = h[3] = len; fwrite(h, 4, 4, g_pcap); fwrite(f, 1, len, g_pcap); fflush(g_pcap);
}
void ToGuest(const uint8_t* f, uint32_t len) { Pcap(f, len); NvnetReceive(f, len); }

// ---- LAN relay (System Link over the Internet): XBOX_RELAY=host:port. Every Ethernet frame that is not meant for our own router is sent to a relay server (tools/relay.py),
// which hubs it to the other consoles; frames from the relay go straight to the guest. Datagram = "XRL1" + frame; a bare "XRL1" is a keepalive/registration.
SOCKET g_relay = INVALID_SOCKET; sockaddr_in g_relayAddr; bool g_relayOn = false;
bool ForRelay(const uint8_t* f, uint32_t len) {   // frames that belong on the virtual LAN rather than to the user-mode router
  if (!g_relayOn || len < 14) return false;
  if (memcmp(f, kGwMac, 6) == 0) return false;                       // unicast to our gateway
  return true;
}
void RelaySend(const uint8_t* f, uint32_t len) { if (g_relay == INVALID_SOCKET) return; std::vector<uint8_t> d(4 + len); memcpy(d.data(), "XRL1", 4); memcpy(d.data() + 4, f, len); sendto(g_relay, (const char*)d.data(), (int)d.size(), 0, (sockaddr*)&g_relayAddr, sizeof g_relayAddr); }
uint32_t RelayThread(void* ) {
  uint8_t buf[2200]; uint32_t lastKeep = 0;
  for (;;) {
    if ((uint32_t)plat::TickMs() - lastKeep > 4000) { lastKeep = (uint32_t)plat::TickMs(); sendto(g_relay, "XRL1", 4, 0, (sockaddr*)&g_relayAddr, sizeof g_relayAddr); }
    fd_set rs; FD_ZERO(&rs); FD_SET(g_relay, &rs); timeval tv{1, 0}; if (select((int)g_relay + 1, &rs, nullptr, nullptr, &tv) <= 0) continue;
    int n = recv(g_relay, (char*)buf, sizeof buf, 0); if (n > 18 && !memcmp(buf, "XRL1", 4)) ToGuest(buf + 4, (uint32_t)n - 4);
  }
}
void RelayStart() {
  const char* r = getenv("XBOX_RELAY"); if (!r || !*r) return;
  char host[200]; int port = 3075; if (sscanf(r, "%199[^:]:%d", host, &port) < 1) return;
  addrinfo hints = {}, *res = nullptr; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; char ps[16]; sprintf(ps, "%d", port);
  if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { Log("[net] relay %s: cannot resolve", r); return; }
  memcpy(&g_relayAddr, res->ai_addr, sizeof g_relayAddr); freeaddrinfo(res);
  g_relay = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); g_relayOn = true; plat::ThreadSpawn(RelayThread, nullptr);
  if (const char* g = getenv("XBOX_GUEST_IP")) { in_addr a; if (inet_pton(AF_INET, g, &a) == 1) kGuestIp = ntohl(a.s_addr); }
  Log("[net] LAN relay %s up, guest IP %u.%u.%u.%u", r, kGuestIp >> 24, (kGuestIp >> 16) & 255, (kGuestIp >> 8) & 255, kGuestIp & 255);
}


// Build and deliver an Ethernet+IPv4 frame to the guest
void SendIp(uint32_t srcIp, uint32_t dstIp, uint8_t proto, const uint8_t* payload, uint32_t plen, const uint8_t* dstMac = nullptr) {
  std::vector<uint8_t> f(14 + 20 + plen); uint8_t* e = f.data();
  memcpy(e, dstMac ? dstMac : g_guestMac, 6); memcpy(e + 6, kGwMac, 6); Wr16(e + 12, 0x0800);
  uint8_t* ip = e + 14; ip[0] = 0x45; ip[1] = 0; Wr16(ip + 2, (uint16_t)(20 + plen)); static uint16_t id = 1; Wr16(ip + 4, id++); Wr16(ip + 6, 0x4000); ip[8] = 64; ip[9] = proto; Wr32(ip + 12, srcIp); Wr32(ip + 16, dstIp);
  Wr16(ip + 10, 0); Wr16(ip + 10, Csum(ip, 20)); memcpy(ip + 20, payload, plen); ToGuest(f.data(), (uint32_t)f.size());
}
void SendUdp(uint32_t srcIp, uint16_t srcPort, uint32_t dstIp, uint16_t dstPort, const uint8_t* data, uint32_t len, const uint8_t* dstMac = nullptr) {
  std::vector<uint8_t> u(8 + len); Wr16(&u[0], srcPort); Wr16(&u[2], dstPort); Wr16(&u[4], (uint16_t)(8 + len)); Wr16(&u[6], 0); memcpy(&u[8], data, len);
  uint8_t ph[12]; Wr32(ph, srcIp); Wr32(ph + 4, dstIp); ph[8] = 0; ph[9] = 17; Wr16(ph + 10, (uint16_t)(8 + len));
  uint32_t s = 0; for (int i = 0; i < 12; i += 2) s += ph[i] << 8 | ph[i + 1];
  uint16_t c = Csum(u.data(), u.size(), s); if (c == 0) c = 0xFFFF; Wr16(&u[6], c);
  SendIp(srcIp, dstIp, 17, u.data(), (uint32_t)u.size(), dstMac);
}

// ---- ARP ----
void HandleArp(const uint8_t* f, uint32_t len) {
  if (len < 42) return; const uint8_t* a = f + 14; if (Rd16(a) != 1 || Rd16(a + 2) != 0x0800 || Rd16(a + 6) != 1) return;   // ethernet/IPv4 request
  uint32_t senderIp = Rd32(a + 14), targetIp = Rd32(a + 24);
  memcpy(g_guestMac, a + 8, 6); if (senderIp) g_guestIpSeen = senderIp;
  NL("ARP who-has %u.%u.%u.%u from %u.%u.%u.%u", targetIp >> 24, (targetIp >> 16) & 255, (targetIp >> 8) & 255, targetIp & 255, senderIp >> 24, (senderIp >> 16) & 255, (senderIp >> 8) & 255, senderIp & 255);
  if (g_relayOn && targetIp != kGwIp && targetIp != kDnsIp) return;   // peers on the relay LAN answer for themselves
  if ((targetIp & kMask) != (kGwIp & kMask) || targetIp == senderIp || targetIp == kGuestIp) return;   // only answer for addresses on our subnet (everything but the guest itself)
  uint8_t r[42]; memcpy(r, f + 6, 6); memcpy(r + 6, kGwMac, 6); Wr16(r + 12, 0x0806); uint8_t* p = r + 14; Wr16(p, 1); Wr16(p + 2, 0x0800); p[4] = 6; p[5] = 4; Wr16(p + 6, 2);
  memcpy(p + 8, kGwMac, 6); Wr32(p + 14, targetIp); memcpy(p + 18, a + 8, 6); Wr32(p + 24, senderIp); ToGuest(r, 42);
}

// ---- DHCP ----
void HandleDhcp(const uint8_t* d, uint32_t len) {
  if (len < 240 || d[0] != 1) return; uint8_t type = 0; const uint8_t* o = d + 240;
  while (o < d + len && *o != 255) { if (*o == 0) { ++o; continue; } if (*o == 53 && o[1] == 1) type = o[2]; o += 2 + o[1]; }
  if (type != 1 && type != 3) return;   // Discover / Request
  NL("DHCP %s", type == 1 ? "discover" : "request");
  std::vector<uint8_t> r(300, 0); r[0] = 2; r[1] = 1; r[2] = 6; memcpy(&r[4], d + 4, 4); memcpy(&r[10], d + 10, 2); Wr32(&r[16], kGuestIp); Wr32(&r[20], kGwIp); memcpy(&r[28], d + 28, 6);
  Wr32(&r[236], 0x63825363); uint8_t* p = &r[240];
  *p++ = 53; *p++ = 1; *p++ = type == 1 ? 2 : 5; *p++ = 54; *p++ = 4; Wr32(p, kGwIp); p += 4; *p++ = 51; *p++ = 4; Wr32(p, 86400); p += 4;
  *p++ = 1; *p++ = 4; Wr32(p, kMask); p += 4; *p++ = 3; *p++ = 4; Wr32(p, kGwIp); p += 4; *p++ = 6; *p++ = 4; Wr32(p, kDnsIp); p += 4; *p++ = 28; *p++ = 4; Wr32(p, 0x0A0002FF); p += 4; *p++ = 255;
  uint8_t bc[6] = {255, 255, 255, 255, 255, 255}; memcpy(g_guestMac, d + 28, 6);
  SendUdp(kGwIp, 67, 0xFFFFFFFF, 68, r.data(), (uint32_t)(p - r.data()), bc);
}

// ---- DNS proxy: forward to the upstream server on a worker thread ----
uint32_t UpstreamDns() {
  static uint32_t ip = 0; if (ip) return ip;
  if (const char* e = getenv("XBOX_DNS")) { in_addr a; if (inet_pton(AF_INET, e, &a) == 1) return ip = ntohl(a.s_addr); }
  // Insignia DNS server (insignia.live/guide/connect). It answers the Xbox Live names with Insignia servers; there is deliberately no fallback to a public resolver, so
  // Microsoft Xbox Live names are never contacted. XBOX_DNS overrides it for offline/LAN testing.
  ip = (46u << 24) | (101u << 16) | (64u << 8) | 175u; return ip;
}
struct DnsJob { std::vector<uint8_t> q; uint16_t guestPort; };
uint32_t DnsThread(void* p) {
  DnsJob* j = (DnsJob*)p; SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = htonl(UpstreamDns());
  NetSetRecvTimeoutMs(s, 3000); sendto(s, (char*)j->q.data(), (int)j->q.size(), 0, (sockaddr*)&a, sizeof a);
  uint8_t buf[1500]; int n = recv(s, (char*)buf, sizeof buf, 0); closesocket(s);
  if (n > 0) { NL("DNS answer %d bytes for guest port %u", n, j->guestPort); SendUdp(kDnsIp, 53, kGuestIp, j->guestPort, buf, (uint32_t)n); } else NL("DNS query timed out");
  delete j; return 0;
}
void HandleDns(const uint8_t* d, uint32_t len, uint16_t guestPort) {
  if (len < 12) return; { char name[128] = ""; int o = 12, k = 0; while (o < (int)len && d[o] && k < 120) { int l = d[o++]; if (k) name[k++] = '.'; for (int i = 0; i < l && o < (int)len && k < 120; i++) name[k++] = (char)d[o++]; } name[k] = 0; NL("DNS query '%s' type %u", name, o + 2 < (int)len ? Rd16(d + o + 1) : 0); }
  DnsJob* j = new DnsJob{std::vector<uint8_t>(d, d + len), guestPort}; plat::ThreadSpawn(DnsThread, j);
}

// ---- UDP NAT: one host socket + receive thread per (guest port, remote ip, remote port) flow ----
struct Flow { SOCKET s; uint16_t guestPort; uint32_t rip; uint16_t rport; uint32_t last; };
std::map<uint64_t, Flow*> g_flows; std::mutex g_flowMx;
uint32_t FlowThread(void* p) {
  Flow* f = (Flow*)p; uint8_t buf[2048];
  for (;;) {
    fd_set rs; FD_ZERO(&rs); FD_SET(f->s, &rs); timeval tv = {5, 0}; int r = select((int)f->s + 1, &rs, nullptr, nullptr, &tv);
    if (r < 0) break; if (r == 0) { if ((uint32_t)plat::TickMs() - f->last > 120000) break; continue; }
    sockaddr_in from; socklen_t fl = sizeof from; int n = recvfrom(f->s, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl); if (n <= 0) continue; f->last = (uint32_t)plat::TickMs();
    NL("UDP %u.%u.%u.%u:%u -> guest:%u %d bytes", ntohl(from.sin_addr.s_addr) >> 24, (ntohl(from.sin_addr.s_addr) >> 16) & 255, (ntohl(from.sin_addr.s_addr) >> 8) & 255, ntohl(from.sin_addr.s_addr) & 255, ntohs(from.sin_port), f->guestPort, n);
    SendUdp(ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), kGuestIp, f->guestPort, buf, (uint32_t)n);
  }
  std::lock_guard<std::mutex> l(g_flowMx); uint64_t key = (uint64_t)f->guestPort << 48 | (uint64_t)f->rip << 16 | f->rport; g_flows.erase(key); closesocket(f->s); delete f; return 0;
}
void HandleUdpNat(uint32_t dstIp, uint16_t srcPort, uint16_t dstPort, const uint8_t* d, uint32_t len) {
  uint64_t key = (uint64_t)srcPort << 48 | (uint64_t)dstIp << 16 | dstPort; Flow* f = nullptr;
  { std::lock_guard<std::mutex> l(g_flowMx); auto it = g_flows.find(key); if (it != g_flows.end()) f = it->second;
    else { SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (s == INVALID_SOCKET) return; sockaddr_in b = {}; b.sin_family = AF_INET; bind(s, (sockaddr*)&b, sizeof b); f = new Flow{s, srcPort, dstIp, dstPort, (uint32_t)plat::TickMs()}; g_flows[key] = f; plat::ThreadSpawn(FlowThread, f);
      NL("UDP flow guest:%u -> %u.%u.%u.%u:%u", srcPort, dstIp >> 24, (dstIp >> 16) & 255, (dstIp >> 8) & 255, dstIp & 255, dstPort); } }
  sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(dstPort); a.sin_addr.s_addr = htonl(dstIp); f->last = (uint32_t)plat::TickMs(); sendto(f->s, (const char*)d, (int)len, 0, (sockaddr*)&a, sizeof a);
}

#include "net_tcp.inc"

void HandleIpv4(const uint8_t* f, uint32_t len) {
  if (len < 34) return; const uint8_t* ip = f + 14; if ((ip[0] >> 4) != 4) return; uint32_t ihl = (ip[0] & 15) * 4, tot = Rd16(ip + 2); if (tot + 14 > len) tot = len - 14; if (ihl < 20 || tot < ihl) return;
  uint8_t proto = ip[9]; uint32_t src = Rd32(ip + 12), dst = Rd32(ip + 16); const uint8_t* l4 = ip + ihl; uint32_t l4len = tot - ihl;
  memcpy(g_guestMac, f + 6, 6); if (src) g_guestIpSeen = src;
  if (proto == 17 && l4len >= 8) {
    uint16_t sp = Rd16(l4), dp = Rd16(l4 + 2), ul = Rd16(l4 + 4); if (ul > l4len) ul = (uint16_t)l4len; const uint8_t* data = l4 + 8; uint32_t dl = ul - 8;
    if (dp == 67) { HandleDhcp(data, dl); return; }
    if (dst == kDnsIp && dp == 53) { HandleDns(data, dl, sp); return; }
    if (dst == 0xFFFFFFFF || dst == 0x0A0002FF) { HandleBroadcast(sp, dp, data, dl); return; }
    if ((dst & 0xF0000000) == 0xE0000000) { NL("UDP multicast %u -> %u (%u bytes) dropped", sp, dp, dl); return; }
    HandleUdpNat(dst, sp, dp, data, dl); return;
  }
  if (proto == 6) { HandleTcp(dst, l4, l4len); return; }
  if (proto == 1 && l4len >= 8 && l4[0] == 8 && dst == kGwIp) {   // ICMP echo to the gateway
    std::vector<uint8_t> r(l4, l4 + l4len); r[0] = 0; Wr16(&r[2], 0); Wr16(&r[2], Csum(r.data(), r.size())); SendIp(kGwIp, src, 1, r.data(), (uint32_t)r.size()); return;
  }
  NL("IPv4 proto %u %u.%u.%u.%u -> %u.%u.%u.%u (%u bytes) not handled", proto, src >> 24, (src >> 16) & 255, (src >> 8) & 255, src & 255, dst >> 24, (dst >> 16) & 255, (dst >> 8) & 255, dst & 255, l4len);
}
}  // namespace

void NetBackendLinkUp() {
  static bool once = false; if (once) return; once = true;
  if (!g_wsa) { NetSockInit(); g_wsa = true; }
  if (const char* p = getenv("XBOX_NET_PCAP")) { g_pcap = fopen(p, "wb"); if (g_pcap) { uint32_t h[6] = {0xA1B2C3D4, 0x00040002, 0, 0, 65535, 1}; h[1] = 0x00040002; fwrite(h, 4, 6, g_pcap); } }
  uint8_t mac[6]; NvnetMac(mac); memcpy(g_guestMac, mac, 6); RelayStart();
  Log("[net] backend up: gateway 10.0.2.2, DNS 10.0.2.3 -> %u.%u.%u.%u (guest MAC %02x:%02x:%02x:%02x:%02x:%02x)", UpstreamDns() >> 24, (UpstreamDns() >> 16) & 255, (UpstreamDns() >> 8) & 255, UpstreamDns() & 255, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void NetBackendTx(const uint8_t* f, uint32_t len) {
  if (!g_wsa) NetBackendLinkUp();
  if (len < 14) return; Pcap(f, len);
  uint16_t type = Rd16(f + 12);
  if (g_relayOn) {   // virtual LAN: everything not meant for our router goes to the relay
    bool toGw = memcmp(f, kGwMac, 6) == 0, bc = (f[0] & 1) != 0;
    bool dhcp = type == 0x0800 && len >= 44 && f[23] == 17 && Rd16(f + 36) == 67;
    bool arpGw = type == 0x0806 && len >= 42 && (Rd32(f + 38) == kGwIp || Rd32(f + 38) == kDnsIp);
    if (!toGw && !dhcp && !arpGw) RelaySend(f, len);
    if (!toGw && !bc) return;                      // unicast to a peer: not for the router
    if (bc && !dhcp && !arpGw) return;             // other broadcasts live on the relay only
  }
  if (type == 0x0806) HandleArp(f, len); else if (type == 0x0800) HandleIpv4(f, len); else NL("ethertype 0x%04x (%u bytes) ignored", type, len);
}
