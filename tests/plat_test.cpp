// Behavioural tests for the platform layer (src/plat.h). The same program runs on every platform in CI: the guest-facing kernel code relies on
// Windows wait semantics (auto/manual events, semaphore counts, recursive mutexes, wait-any/all, pulse), so the POSIX backend must match them.
#include "plat.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static void TestTime() {
  uint64_t a = plat::TickMs(); plat::SleepMs(30); uint64_t b = plat::TickMs();
  CHECK(b >= a + 20 && b < a + 2000);
  uint64_t p0 = plat::PerfCounter(); plat::SleepMs(10); uint64_t p1 = plat::PerfCounter();
  CHECK(p1 > p0 && plat::PerfFreq() > 1000000);
  uint64_t ft = plat::SystemTime100ns(); CHECK(ft > 130000000000000000ull);   // after 2012
}

static void TestEvents() {
  plat::Obj* a = plat::EventNew(false, false);                       // auto-reset
  CHECK(plat::Wait(a, 0) == plat::kWaitTimeout);
  plat::EventSet(a); CHECK(plat::Wait(a, 0) == plat::kWaitOk); CHECK(plat::Wait(a, 0) == plat::kWaitTimeout);   // consumed
  plat::Obj* m = plat::EventNew(true, true);                         // manual-reset, initially set
  CHECK(plat::Wait(m, 0) == plat::kWaitOk); CHECK(plat::Wait(m, 0) == plat::kWaitOk);
  plat::EventReset(m); CHECK(plat::Wait(m, 10) == plat::kWaitTimeout);
  // a waiter thread released by a set
  struct Ctx { plat::Obj* ev; std::atomic<int> got{0}; } ctx{plat::EventNew(true, false)};
  plat::Obj* t = plat::ThreadStart([](void* p) -> uint32_t { Ctx* c = (Ctx*)p; if (plat::Wait(c->ev, 5000) == plat::kWaitOk) c->got = 1; return 7; }, &ctx, 0, false, nullptr);
  plat::SleepMs(50); CHECK(ctx.got == 0);
  plat::EventSet(ctx.ev); CHECK(plat::Wait(t, 5000) == plat::kWaitOk); CHECK(ctx.got == 1);
  // pulse wakes current waiters of a manual-reset event and leaves it unsignalled
  ctx.got = 0; plat::EventReset(ctx.ev);
  plat::Obj* t2 = plat::ThreadStart([](void* p) -> uint32_t { Ctx* c = (Ctx*)p; if (plat::Wait(c->ev, 5000) == plat::kWaitOk) c->got = 1; return 0; }, &ctx, 0, false, nullptr);
  plat::SleepMs(100); plat::EventPulse(ctx.ev); CHECK(plat::Wait(t2, 5000) == plat::kWaitOk); CHECK(ctx.got == 1);
  CHECK(plat::Wait(ctx.ev, 10) == plat::kWaitTimeout);
  plat::ObjClose(a); plat::ObjClose(m); plat::ObjClose(ctx.ev); plat::ObjClose(t); plat::ObjClose(t2);
}

static void TestSemaphore() {
  plat::Obj* s = plat::SemaphoreNew(2, 3); int32_t prev = -1;
  CHECK(plat::Wait(s, 0) == plat::kWaitOk); CHECK(plat::Wait(s, 0) == plat::kWaitOk); CHECK(plat::Wait(s, 10) == plat::kWaitTimeout);
  CHECK(plat::SemaphoreRelease(s, 2, &prev)); CHECK(prev == 0);
  CHECK(plat::SemaphoreRelease(s, 1, &prev)); CHECK(prev == 2);
  CHECK(!plat::SemaphoreRelease(s, 1, &prev));                       // would exceed the maximum
  plat::ObjClose(s);
}

static void TestMutex() {
  plat::Obj* m = plat::MutexNew(false);
  CHECK(plat::Wait(m, 0) == plat::kWaitOk); CHECK(plat::Wait(m, 0) == plat::kWaitOk);   // recursive for the owner
  struct Ctx { plat::Obj* m; std::atomic<int> state{0}; } ctx{m};
  plat::Obj* t = plat::ThreadStart([](void* p) -> uint32_t { Ctx* c = (Ctx*)p; c->state = plat::Wait(c->m, 100) == plat::kWaitTimeout ? 1 : 2; return 0; }, &ctx, 0, false, nullptr);
  CHECK(plat::Wait(t, 5000) == plat::kWaitOk); CHECK(ctx.state == 1);   // another thread cannot take it while owned
  CHECK(plat::MutexRelease(m)); CHECK(plat::MutexRelease(m)); CHECK(!plat::MutexRelease(m));   // released twice, third fails
  ctx.state = 0; plat::ObjClose(t);
  t = plat::ThreadStart([](void* p) -> uint32_t { Ctx* c = (Ctx*)p; c->state = plat::Wait(c->m, 1000) == plat::kWaitOk ? 2 : 1; plat::MutexRelease(c->m); return 0; }, &ctx, 0, false, nullptr);
  CHECK(plat::Wait(t, 5000) == plat::kWaitOk); CHECK(ctx.state == 2);
  plat::ObjClose(t); plat::ObjClose(m);
}

static void TestWaitMany() {
  plat::Obj* e1 = plat::EventNew(false, false); plat::Obj* e2 = plat::EventNew(false, false); plat::Obj* both[2] = {e1, e2};
  CHECK(plat::WaitMany(both, 2, false, 10) == plat::kWaitTimeout);
  plat::EventSet(e2); CHECK(plat::WaitMany(both, 2, false, 10) == 1);
  CHECK(plat::WaitMany(both, 2, false, 10) == plat::kWaitTimeout);   // auto-reset consumed
  plat::EventSet(e1); CHECK(plat::WaitMany(both, 2, true, 10) == plat::kWaitTimeout);   // wait-all needs both
  plat::EventSet(e2); CHECK(plat::WaitMany(both, 2, true, 10) == 0);
  CHECK(plat::Wait(e1, 0) == plat::kWaitTimeout && plat::Wait(e2, 0) == plat::kWaitTimeout);   // wait-all consumed both
  plat::Obj* sem = plat::SemaphoreNew(0, 10); plat::Obj* ev = plat::EventNew(false, false);
  CHECK(plat::SignalAndWait(ev, sem, 10) == plat::kWaitTimeout);     // signals the event, then waits on the empty semaphore
  CHECK(plat::Wait(ev, 0) == plat::kWaitOk);
  plat::ObjClose(e1); plat::ObjClose(e2); plat::ObjClose(sem); plat::ObjClose(ev);
}

static void TestThreads() {
  struct Ctx { std::atomic<uint32_t> id{0}; std::atomic<int> ran{0}; } ctx;
  uint32_t childId = 0;
  plat::Obj* t = plat::ThreadStart([](void* p) -> uint32_t { Ctx* c = (Ctx*)p; c->id = plat::ThreadId(); c->ran = 1; return 3; }, &ctx, 0, true, &childId);
  plat::SleepMs(100); CHECK(ctx.ran == 0);                           // created suspended
  CHECK(plat::ThreadResume(t) == 1);
  CHECK(plat::Wait(t, 5000) == plat::kWaitOk); CHECK(ctx.ran == 1);
  CHECK(ctx.id == childId && childId != plat::ThreadId());
  plat::ObjClose(t);
  plat::Obj* dup = plat::ThreadStart([](void*) -> uint32_t { plat::SleepMs(20); return 0; }, nullptr, 1 << 20, false, nullptr);
  plat::Obj* d2 = plat::ObjDup(dup); plat::ObjClose(dup); CHECK(plat::Wait(d2, 5000) == plat::kWaitOk); plat::ObjClose(d2);
}

static void TestVm() {
  const size_t kPage = 0x10000;
  void* r = plat::VmReserve(nullptr, 16 * kPage); CHECK(r != nullptr);
  CHECK(plat::VmCommit(r, kPage, plat::kProtRW));
  uint8_t* p = (uint8_t*)r; p[0] = 0x5A; p[kPage - 1] = 0xA5; CHECK(p[0] == 0x5A && p[kPage - 1] == 0xA5);
  plat::VmInfo vi{}; CHECK(plat::VmQuery(r, &vi) && vi.state == 2);
  CHECK(plat::VmProtect(r, kPage, plat::kProtRead));
  CHECK(plat::VmProtect(r, kPage, plat::kProtRW));
  CHECK(plat::VmFree(r, kPage, MEM_DECOMMIT));                       // decommit then commit again: zero filled
  CHECK(plat::VmCommit(r, kPage, plat::kProtRW)); CHECK(p[0] == 0);
  CHECK(plat::VmReserve(r, kPage) == nullptr);                       // an occupied fixed range cannot be reserved again
  // commit and decommit ranges that are not page aligned (the guest heap grows in 16-byte steps): the OS must round outward like VirtualAlloc does
  CHECK(plat::VmCommit((uint8_t*)r + 2 * kPage + 16, 48, plat::kProtRW)); ((uint8_t*)r)[2 * kPage + 16] = 9; ((uint8_t*)r)[2 * kPage + 63] = 10;
  CHECK(plat::VmAlloc((uint8_t*)r + 3 * kPage + 100, 200, MEM_COMMIT, PAGE_READWRITE) != nullptr); ((uint8_t*)r)[3 * kPage + 299] = 11;
  void* x = plat::VmAlloc(nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);   // must work everywhere (the POSIX layer drops the exec bit: iOS forbids W^X)
  CHECK(x != nullptr); if (x) { ((uint8_t*)x)[0] = 1; ((uint8_t*)x)[kPage - 1] = 2; plat::VmFree(x, 0, MEM_RELEASE); }
  void* c = plat::VmAlloc(nullptr, 3 * kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); CHECK(c != nullptr);
  if (c) { ((uint8_t*)c)[3 * kPage - 1] = 1; CHECK(plat::VmFree(c, 0, MEM_RELEASE)); }
  CHECK(plat::VmFree(r, 0, MEM_RELEASE));
}

static void TestShm() {
  const size_t kSize = 0x100000;
  plat::Shm* s = plat::ShmCreate(kSize); CHECK(s != nullptr);
  if (!s) return;
  void* hole = plat::VmReserve(nullptr, 4 * kSize); CHECK(hole != nullptr);   // find two free windows, then hand them over to the mappings
  if (!hole) return;
  plat::VmFree(hole, 0, MEM_RELEASE);
  uint8_t* a = (uint8_t*)plat::ShmMap(s, 0, kSize, hole);
  uint8_t* b = (uint8_t*)plat::ShmMap(s, 0, kSize, (uint8_t*)hole + 2 * kSize);
  CHECK(a != nullptr && b != nullptr);
  if (a && b) { a[1234] = 0x42; CHECK(b[1234] == 0x42); b[kSize - 1] = 0x77; CHECK(a[kSize - 1] == 0x77); }   // two views of the same RAM
  if (a) plat::ShmUnmap(a, kSize);
  if (b) plat::ShmUnmap(b, kSize);
}

static void TestFiles() {
  const char* dir = "plat_test_dir"; plat::RemoveFile("plat_test_dir/a.bin"); plat::RemoveFile("plat_test_dir/b.bin"); plat::RemoveDir(dir);
  CHECK(plat::MakeDir(dir)); bool isDir = false; CHECK(plat::PathExists(dir, &isDir) && isDir);
  bool pathMissing = false; CHECK(plat::FileOpen("plat_test_dir/missing.bin", false, plat::Disp::OpenExisting, &pathMissing) == nullptr && !pathMissing);
  CHECK(plat::FileOpen("plat_test_nodir/x.bin", false, plat::Disp::OpenExisting, &pathMissing) == nullptr && pathMissing);
  plat::File* f = plat::FileOpen("plat_test_dir/a.bin", true, plat::Disp::CreateAlways, nullptr); CHECK(f != nullptr);
  if (f) {
    const char* msg = "hello, world"; CHECK(plat::FileWriteAt(f, msg, 12, 0) == 12); CHECK(plat::FileWriteAt(f, "!", 1, 100) == 1);
    CHECK(plat::FileSize(f) == 101); char buf[16] = {}; CHECK(plat::FileReadAt(f, buf, 5, 7) == 5 && std::memcmp(buf, "world", 5) == 0);
    CHECK(plat::FileReadAt(f, buf, 4, 200) == 0);                    // past the end
    CHECK(plat::FileFlush(f)); CHECK(plat::FileTruncate(f, 20)); CHECK(plat::FileSize(f) == 20);
    plat::FileClose(f);
  }
  plat::StatInfo si{}; CHECK(plat::PathStat("plat_test_dir/a.bin", &si) && si.size == 20 && !si.isDir && si.written > 130000000000000000ull);
  CHECK(plat::RenameFile("plat_test_dir/a.bin", "plat_test_dir/b.bin")); CHECK(!plat::PathExists("plat_test_dir/a.bin", nullptr));
  auto names = plat::ListDir(dir, "*.BIN"); CHECK(names.size() == 1 && names[0] == "b.bin");   // case-insensitive pattern
  CHECK(plat::ListDir(dir, "x*").empty());
  CHECK(plat::RemoveFile("plat_test_dir/b.bin")); CHECK(plat::RemoveDir(dir));
}

static void TestMisc() {
  uint32_t a = plat::RandomU32(), b = plat::RandomU32(), c = plat::RandomU32(); CHECK(!(a == b && b == c));
  CHECK(plat::PopCount32(0xF0F0F0F0u) == 16);
  volatile int32_t v = 5; CHECK(plat::AtomicAdd(&v, 3) == 5 && v == 8); CHECK(plat::AtomicExchange(&v, 1) == 8); CHECK(plat::AtomicCas(&v, 1, 9) == 1 && v == 9);
  CHECK(plat::AtomicCas(&v, 1, 7) == 9 && v == 9);
  CHECK(plat::AtomicAnd(&v, 8) == 9 && v == 8); CHECK(plat::AtomicOr(&v, 3) == 8 && v == 11); CHECK(plat::AtomicXor(&v, 1) == 11 && v == 10);
}

int main() {
  TestTime(); TestEvents(); TestSemaphore(); TestMutex(); TestWaitMany(); TestThreads(); TestVm(); TestShm(); TestFiles(); TestMisc();
  std::printf("plat_test: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
