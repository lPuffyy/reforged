// Read-only XDVDFS (original Xbox disc) reader: serves files straight from the .iso, no extraction.
#include "kernel.h"
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <windows.h>

namespace {
HANDLE g_iso = INVALID_HANDLE_VALUE;
uint64_t g_base = 0;
std::map<std::string, DiscEntry> g_files;  // lower-case path ('/' separated, no leading slash) -> entry
constexpr uint32_t S = 2048;

bool ReadAt(uint64_t off, void* dst, uint32_t len) {
  OVERLAPPED ov = {}; ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
  DWORD got = 0;
  return ReadFile(g_iso, dst, len, &got, &ov) && got == len;
}
std::string Lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

void Walk(uint32_t sector, uint32_t size, const std::string& prefix) {
  std::vector<uint8_t> d(size);
  if (!ReadAt(g_base + (uint64_t)sector * S, d.data(), size)) return;
  std::vector<uint32_t> stack = {0};
  std::vector<bool> seen(size / 4 + 1, false);
  while (!stack.empty()) {
    uint32_t o = stack.back(); stack.pop_back();
    if (o + 14 > size || seen[o / 4]) continue;
    seen[o / 4] = true;
    uint16_t l, r; uint32_t sec, sz; uint8_t attr, nl;
    memcpy(&l, &d[o], 2); memcpy(&r, &d[o + 2], 2); memcpy(&sec, &d[o + 4], 4); memcpy(&sz, &d[o + 8], 4); attr = d[o + 12]; nl = d[o + 13];
    if (l == 0xFFFF) continue;
    if (l) stack.push_back(l * 4u);
    if (r) stack.push_back(r * 4u);
    std::string name((const char*)&d[o + 14], nl);
    std::string path = prefix + Lower(name);
    if (attr & 0x10) { g_files[path] = {(uint64_t)sec * S, sz, true}; Walk(sec, sz, path + "/"); }
    else g_files[path] = {(uint64_t)sec * S, sz, false};
  }
}
}  // namespace

bool DiscOpen(const char* isoPath) {
  g_iso = CreateFileA(isoPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED * 0, nullptr);
  if (g_iso == INVALID_HANDLE_VALUE) return false;
  for (uint64_t base : {0ull, 0x18300000ull, 0xFD90000ull, 0x2080000ull}) {
    char magic[20];
    if (ReadAt(base + 32 * S, magic, 20) && !memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20)) { g_base = base; break; }
    if (base == 0x2080000ull) return false;
  }
  uint32_t hdr[2];
  ReadAt(g_base + 32 * S + 20, hdr, 8);
  Walk(hdr[0], hdr[1], "");
  Log("[disc] %s: %zu entries, partition base 0x%llx", isoPath, g_files.size(), (unsigned long long)g_base);
  return true;
}
bool DiscFind(const char* path, DiscEntry* out) {
  std::string p = Lower(path);
  for (char& c : p) if (c == '\\') c = '/';
  while (!p.empty() && p[0] == '/') p.erase(0, 1);
  while (!p.empty() && p.back() == '/') p.pop_back();
  if (p.empty()) { *out = {0, 0, true}; return true; }
  auto it = g_files.find(p);
  if (it == g_files.end()) return false;
  *out = it->second; return true;
}
bool DiscRead(uint64_t offset, void* dst, uint32_t len) { return ReadAt(g_base + offset, dst, len); }
void DiscList(const char* dirPath, std::vector<std::string>* names) {
  std::string p = Lower(dirPath);
  for (char& c : p) if (c == '\\') c = '/';
  while (!p.empty() && p[0] == '/') p.erase(0, 1);
  while (!p.empty() && p.back() == '/') p.pop_back();
  std::string prefix = p.empty() ? "" : p + "/";
  for (auto& kv : g_files) {
    const std::string& k = kv.first;
    if (k.compare(0, prefix.size(), prefix) != 0 || k.size() == prefix.size()) continue;
    if (k.find('/', prefix.size()) != std::string::npos) continue;
    names->push_back(k.substr(prefix.size()));
  }
}
