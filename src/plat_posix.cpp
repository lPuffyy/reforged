// POSIX implementation of plat.h (Linux, macOS, iOS). All waitable objects share one mutex + condition variable: simple, correct for
// wait-any/wait-all with timeouts, and fast enough for the call rates a kernel HLE sees.
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "plat.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <vector>
#include <thread>
#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#endif

namespace plat {

// ---- time ----
using Clock = std::chrono::steady_clock;
static Clock::time_point Epoch() { static const Clock::time_point t = Clock::now(); return t; }
uint64_t TickMs() { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - Epoch()).count() + 1000; }
uint64_t PerfCounter() { return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Epoch()).count(); }
uint64_t PerfFreq() { return 1000000000ull; }
uint64_t SystemTime100ns() {
  using namespace std::chrono;
  uint64_t ns = (uint64_t)duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
  return ns / 100 + 116444736000000000ull;   // 1601 -> 1970
}
void SleepMs(uint32_t ms) { if (ms == 0) sched_yield(); else { timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L}; while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {} } }
void YieldThread() { sched_yield(); }
void BeginHighResTimer() {}

// ---- waitable objects ----
enum class Kind { Event, Semaphore, Mutex, Thread };
struct Obj {
  Kind kind; int refs = 1;
  bool manual = false, signaled = false;       // events; threads use `signaled` for "exited"
  uint64_t pulses = 0;                         // EventPulse() count: waiters that were already waiting see it change
  int32_t count = 0, max = 0;                  // semaphores
  uint32_t owner = 0; int32_t recursion = 0;   // mutexes
  int32_t suspended = 0;                       // threads created suspended
};
static std::mutex& M() { static std::mutex m; return m; }
static std::condition_variable& CV() { static std::condition_variable c; return c; }
static std::atomic<uint32_t> g_nextTid{1};
static thread_local uint32_t t_tid = 0;
static thread_local Obj* t_thread = nullptr;
uint32_t ThreadId() { if (!t_tid) t_tid = g_nextTid++; return t_tid; }

// Caller holds M(). Takes the object if it is available to `self`.
static bool TryAcquire(Obj* o, uint32_t self) {
  switch (o->kind) {
    case Kind::Event: if (!o->signaled) return false; if (!o->manual) o->signaled = false; return true;
    case Kind::Semaphore: if (o->count <= 0) return false; --o->count; return true;
    case Kind::Mutex: if (o->owner != 0 && o->owner != self) return false; o->owner = self; ++o->recursion; return true;
    case Kind::Thread: return o->signaled;
  }
  return false;
}
static bool CanAcquire(Obj* o, uint32_t self) {
  switch (o->kind) {
    case Kind::Event: case Kind::Thread: return o->signaled;
    case Kind::Semaphore: return o->count > 0;
    case Kind::Mutex: return o->owner == 0 || o->owner == self;
  }
  return false;
}
static void Unref(Obj* o) { if (--o->refs == 0) delete o; }   // caller holds M()

struct ThreadCtx { ThreadFn fn; void* arg; Obj* obj; uint32_t id; };
static void* ThreadTramp(void* p) {
  ThreadCtx c = *(ThreadCtx*)p; delete (ThreadCtx*)p;
  t_tid = c.id; t_thread = c.obj;
  { std::unique_lock<std::mutex> l(M()); CV().wait(l, [&] { return c.obj->suspended == 0; }); }
  uint32_t code = c.fn(c.arg);
  ThreadExit(code);
  return nullptr;
}
Obj* ThreadStart(ThreadFn fn, void* arg, size_t stackSize, bool suspended, uint32_t* idOut) {
  Obj* o = new Obj{}; o->kind = Kind::Thread; o->refs = 2; o->suspended = suspended ? 1 : 0;   // one reference for the creator's handle, one for the thread itself
  uint32_t id = g_nextTid++; if (idOut) *idOut = id;
  pthread_attr_t at; pthread_attr_init(&at); if (stackSize) pthread_attr_setstacksize(&at, std::max<size_t>(stackSize, 256 * 1024));
  pthread_t th; int rc = pthread_create(&th, &at, ThreadTramp, new ThreadCtx{fn, arg, o, id}); pthread_attr_destroy(&at);
  if (rc != 0) { delete o; return nullptr; }
  pthread_detach(th);
  return o;
}
uint32_t ThreadResume(Obj* t) {
  std::lock_guard<std::mutex> l(M()); int32_t prev = t->suspended;
  if (prev > 0) { --t->suspended; if (t->suspended == 0) CV().notify_all(); }
  return (uint32_t)prev;
}
void ThreadExit(uint32_t) {
  if (t_thread) { std::lock_guard<std::mutex> l(M()); t_thread->signaled = true; Obj* o = t_thread; t_thread = nullptr; Unref(o); CV().notify_all(); }
  pthread_exit(nullptr);
}

Obj* EventNew(bool manual, bool set) { Obj* o = new Obj{}; o->kind = Kind::Event; o->manual = manual; o->signaled = set; return o; }
Obj* SemaphoreNew(int32_t initial, int32_t maximum) { Obj* o = new Obj{}; o->kind = Kind::Semaphore; o->count = initial; o->max = maximum; return o; }
Obj* MutexNew(bool owned) { Obj* o = new Obj{}; o->kind = Kind::Mutex; if (owned) { o->owner = ThreadId(); o->recursion = 1; } return o; }
bool EventSet(Obj* o) { { std::lock_guard<std::mutex> l(M()); o->signaled = true; } CV().notify_all(); return true; }
bool EventReset(Obj* o) { std::lock_guard<std::mutex> l(M()); o->signaled = false; return true; }
bool EventPulse(Obj* o) { { std::lock_guard<std::mutex> l(M()); ++o->pulses; } CV().notify_all(); return true; }
bool SemaphoreRelease(Obj* o, int32_t n, int32_t* prev) {
  { std::lock_guard<std::mutex> l(M()); if (prev) *prev = o->count; if (n <= 0 || (int64_t)o->count + n > o->max) return false; o->count += n; }
  CV().notify_all(); return true;
}
bool MutexRelease(Obj* o) {
  { std::lock_guard<std::mutex> l(M()); if (o->owner != ThreadId() || o->recursion <= 0) return false; if (--o->recursion == 0) o->owner = 0; }
  CV().notify_all(); return true;
}
Obj* ObjDup(Obj* o) { std::lock_guard<std::mutex> l(M()); ++o->refs; return o; }
void ObjClose(Obj* o) { if (!o) return; std::lock_guard<std::mutex> l(M()); Unref(o); }

uint32_t WaitMany(Obj* const* objs, uint32_t count, bool all, uint32_t ms) {
  uint32_t self = ThreadId();
  std::unique_lock<std::mutex> l(M());
  std::vector<uint64_t> pulse0(count); for (uint32_t i = 0; i < count; i++) pulse0[i] = objs[i]->pulses;
  auto pulsed = [&](uint32_t i) { return objs[i]->kind == Kind::Event && objs[i]->pulses != pulse0[i]; };
  auto attempt = [&]() -> int64_t {
    if (all) {
      for (uint32_t i = 0; i < count; i++) if (!CanAcquire(objs[i], self) && !pulsed(i)) return -1;
      for (uint32_t i = 0; i < count; i++) if (!pulsed(i)) TryAcquire(objs[i], self);
      return 0;
    }
    for (uint32_t i = 0; i < count; i++) { if (pulsed(i)) return (int64_t)i; if (TryAcquire(objs[i], self)) return (int64_t)i; }
    return -1;
  };
  int64_t r = attempt();
  if (r >= 0) return (uint32_t)r;
  if (ms == 0) return kWaitTimeout;
  if (ms == kInfinite) { for (;;) { CV().wait(l); r = attempt(); if (r >= 0) return (uint32_t)r; } }
  auto deadline = Clock::now() + std::chrono::milliseconds(ms);
  while (CV().wait_until(l, deadline) != std::cv_status::timeout) { r = attempt(); if (r >= 0) return (uint32_t)r; }
  r = attempt(); return r >= 0 ? (uint32_t)r : kWaitTimeout;
}
uint32_t SignalAndWait(Obj* sig, Obj* wait, uint32_t ms) {
  switch (sig->kind) { case Kind::Event: EventSet(sig); break; case Kind::Semaphore: SemaphoreRelease(sig, 1, nullptr); break; case Kind::Mutex: MutexRelease(sig); break; default: break; }
  return WaitMany(&wait, 1, false, ms) == 0 ? kWaitOk : kWaitTimeout;
}
uint32_t Wait(Obj* o, uint32_t ms) { return WaitMany(&o, 1, false, ms) == 0 ? kWaitOk : kWaitTimeout; }

// ---- virtual memory ----
static std::mutex& VmM() { static std::mutex m; return m; }
static std::map<uintptr_t, size_t>& Reservations() { static std::map<uintptr_t, size_t> r; return r; }
// Windows rounds commit/protect/decommit ranges outward to whole pages; mprotect/mmap require the start to be page aligned, so do the same here.
static void PageRound(void*& at, size_t& size) {
  uintptr_t pg = (uintptr_t)sysconf(_SC_PAGESIZE), a = (uintptr_t)at, start = a & ~(pg - 1), end = (a + size + pg - 1) & ~(pg - 1);
  at = (void*)start; size = (size_t)(end - start);
}
// Executable mappings are never requested: translated code is ordinary native code, and iOS / Apple Silicon refuse writable+executable pages (W^X), so the exec bit is dropped.
static int ProtFlags(int p) { int f = 0; if (p & kProtRead) f |= PROT_READ; if (p & kProtWrite) f |= PROT_WRITE | PROT_READ; return f; }
void* VmReserve(void* at, size_t size) {
  void* p = mmap(at, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (at && p != at) { munmap(p, size); return nullptr; }      // the requested range was taken: fail like VirtualAlloc does
  std::lock_guard<std::mutex> l(VmM()); Reservations()[(uintptr_t)p] = size;
  return p;
}
bool VmCommit(void* at, size_t size, int prot) { PageRound(at, size); return mprotect(at, size, ProtFlags(prot)) == 0; }
bool VmProtect(void* at, size_t size, int prot) { PageRound(at, size); return mprotect(at, size, ProtFlags(prot)) == 0; }
bool VmRelease(void* at, size_t size) {
  if (size) { PageRound(at, size); return mmap(at, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0) != MAP_FAILED; }   // decommit: pages read as zero when committed again
  std::lock_guard<std::mutex> l(VmM()); auto it = Reservations().find((uintptr_t)at); if (it == Reservations().end()) return false;
  munmap(at, it->second); Reservations().erase(it); return true;
}
static int ProtFromWin(uint32_t p) {
  switch (p & 0xFF) { case PAGE_NOACCESS: return kProtNone; case PAGE_READONLY: return kProtRead; case PAGE_READWRITE: case PAGE_WRITECOPY: return kProtRW; case PAGE_EXECUTE: return kProtExec;
                      case PAGE_EXECUTE_READ: return kProtRead | kProtExec; case PAGE_EXECUTE_READWRITE: return kProtRWX; default: return kProtRW; }
}
void* VmAlloc(void* at, size_t size, uint32_t type, uint32_t prot) {
  int pr = ProtFromWin(prot);
  if (!at || (type & MEM_RESERVE)) { void* p = VmReserve(at, size); if (!p) return nullptr; if ((type & MEM_COMMIT) && !VmCommit(p, size, pr)) { VmRelease(p, 0); return nullptr; } return p; }
  return VmCommit(at, size, pr) ? at : nullptr;      // commit inside an existing reservation
}
bool VmFree(void* at, size_t size, uint32_t type) { return (type & MEM_RELEASE) ? VmRelease(at, 0) : VmRelease(at, size); }
bool VmProtectWin(void* at, size_t size, uint32_t prot) { return VmProtect(at, size, ProtFromWin(prot)); }
bool VmQuery(const void* at, VmInfo* out) {
  // No portable protection query: report whether the page is mapped at all (enough to reject wild guest pointers).
  size_t pg = (size_t)sysconf(_SC_PAGESIZE); uintptr_t base = (uintptr_t)at & ~(uintptr_t)(pg - 1);
#ifdef __APPLE__
  char vec = 0;           // macOS declares the vector as char*
#else
  unsigned char vec = 0;
#endif
  out->base = (void*)base; out->size = pg;
  if (mincore((void*)base, pg, &vec) != 0) { out->state = 0; out->prot = 0; return true; }
  out->state = 2; out->prot = kProtRW; return true;
}

struct Shm { int fd; size_t size; };
Shm* ShmCreate(size_t size) {
  int fd = -1;
#if defined(__linux__) && defined(SYS_memfd_create)
  fd = (int)syscall(SYS_memfd_create, "guest-ram", 0);
#endif
  if (fd < 0) { char tmpl[] = "/tmp/reforged-shm-XXXXXX"; const char* td = getenv("TMPDIR"); std::string t = td ? std::string(td) + "/reforged-shm-XXXXXX" : std::string(tmpl); std::vector<char> b(t.begin(), t.end()); b.push_back(0); fd = mkstemp(b.data()); if (fd >= 0) unlink(b.data()); }
  if (fd < 0 || ftruncate(fd, (off_t)size) != 0) { if (fd >= 0) close(fd); return nullptr; }
  return new Shm{fd, size};
}
void* ShmMap(Shm* s, size_t offset, size_t size, void* at) {
  void* p = mmap(at, size, PROT_READ | PROT_WRITE, MAP_SHARED | (at ? MAP_FIXED : 0), s->fd, (off_t)offset);
  return p == MAP_FAILED ? nullptr : p;
}
void ShmUnmap(void* at, size_t size) { munmap(at, size); }

// ---- files ----
struct File { int fd; };
File* FileOpen(const char* path, bool write, Disp d, bool* pathNotFound) {
  int fl = (write ? O_RDWR : O_RDONLY) | O_CLOEXEC;
  switch (d) { case Disp::OpenExisting: break; case Disp::CreateNew: fl |= O_CREAT | O_EXCL; break; case Disp::CreateAlways: fl |= O_CREAT | O_TRUNC; break; case Disp::OpenAlways: fl |= O_CREAT; break; case Disp::TruncateExisting: fl |= O_TRUNC; break; }
  int fd = open(path, fl, 0666);
  if (fd < 0) {
    if (pathNotFound) { *pathNotFound = false; if (errno == ENOENT || errno == ENOTDIR) { std::string p(path); size_t s = p.find_last_of('/'); bool dir = false; *pathNotFound = s != std::string::npos && !PathExists(p.substr(0, s).c_str(), &dir); } }
    return nullptr;
  }
  return new File{fd};
}
int64_t FileReadAt(File* f, void* buf, uint32_t len, uint64_t off) { ssize_t n; do { n = pread(f->fd, buf, len, (off_t)off); } while (n < 0 && errno == EINTR); return n; }
int64_t FileWriteAt(File* f, const void* buf, uint32_t len, uint64_t off) { ssize_t n; do { n = pwrite(f->fd, buf, len, (off_t)off); } while (n < 0 && errno == EINTR); return n; }
bool FileFlush(File* f) { return fsync(f->fd) == 0; }
bool FileTruncate(File* f, uint64_t size) { return ftruncate(f->fd, (off_t)size) == 0; }
uint64_t FileSize(File* f) { struct stat st; return fstat(f->fd, &st) == 0 ? (uint64_t)st.st_size : 0; }
void FileClose(File* f) { if (f) { close(f->fd); delete f; } }
bool PathExists(const char* path, bool* isDir) { struct stat st; if (stat(path, &st) != 0) return false; if (isDir) *isDir = S_ISDIR(st.st_mode); return true; }
bool PathStat(const char* path, StatInfo* o) {
  struct stat st; if (stat(path, &st) != 0) return false;
  auto ft = [](time_t t) { return (uint64_t)t * 10000000ull + 116444736000000000ull; };
  o->size = (uint64_t)st.st_size; o->isDir = S_ISDIR(st.st_mode); o->created = ft(st.st_ctime); o->accessed = ft(st.st_atime); o->written = ft(st.st_mtime); return true;
}
bool MakeDir(const char* path) { return mkdir(path, 0777) == 0; }
bool RemoveDir(const char* path) { return rmdir(path) == 0; }
bool RemoveFile(const char* path) { return unlink(path) == 0; }
bool RenameFile(const char* from, const char* to) { return rename(from, to) == 0; }
static bool Wild(const char* p, const char* s) {   // case-insensitive * and ? match
  for (; *p; p++, s++) {
    if (*p == '*') { while (p[1] == '*') p++; if (!p[1]) return true; for (; *s; s++) if (Wild(p + 1, s)) return true; return Wild(p + 1, s); }
    if (!*s) return false;
    if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return false;
  }
  return !*s;
}
std::vector<std::string> ListDir(const char* dir, const char* pattern) {
  std::vector<std::string> out; DIR* d = opendir(dir); if (!d) return out;
  while (dirent* e = readdir(d)) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; if (Wild(pattern, e->d_name)) out.push_back(e->d_name); }
  closedir(d); std::sort(out.begin(), out.end()); return out;
}

void* LoadLib(const char* name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void* LibSym(void* lib, const char* name) { return dlsym(lib, name); }

struct AudioCtx { uint32_t rate, frames; AudioFn fn; void* user; };
static uint32_t AudioThread(void* p) {
  AudioCtx c = *(AudioCtx*)p; delete (AudioCtx*)p;
  std::vector<int16_t> buf((size_t)c.frames * 2); auto next = Clock::now(); auto step = std::chrono::nanoseconds((uint64_t)c.frames * 1000000000ull / c.rate);
  for (;;) { c.fn(buf.data(), c.user); next += step; std::this_thread::sleep_until(next); }   // no device: consume at real-time pace so the game's audio clock behaves
}
bool AudioOutStart(uint32_t rate, uint32_t frames, uint32_t, AudioFn fn, void* user) { ThreadSpawn(AudioThread, new AudioCtx{rate, frames, fn, user}); return true; }

void SetEnv(const char* name, const char* value) { setenv(name, value, 1); }
bool SpawnSelf(const std::vector<std::string>& args) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  (void)args; return false;   // iOS apps cannot start processes
#else
  char exe[4096];
#ifdef __APPLE__
  uint32_t sz = sizeof exe; if (_NSGetExecutablePath(exe, &sz) != 0) return false;
#else
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1); if (n <= 0) return false; exe[n] = 0;
#endif
  pid_t pid = fork(); if (pid < 0) return false;
  if (pid == 0) { std::vector<char*> av; av.push_back(exe); for (auto& a : args) av.push_back(const_cast<char*>(a.c_str())); av.push_back(nullptr); execv(exe, av.data()); _exit(127); }
  return true;
#endif
}

uint32_t RandomU32() { uint32_t v = 0; FILE* f = fopen("/dev/urandom", "rb"); if (f) { if (fread(&v, 1, 4, f) != 4) v = (uint32_t)PerfCounter(); fclose(f); } else v = (uint32_t)PerfCounter(); return v; }

static CrashFn g_crash;
static void CrashSig(int sig, siginfo_t* si, void* uc) {
  (void)uc; if (g_crash) g_crash(sig == SIGSEGV || sig == SIGBUS ? "access violation" : sig == SIGILL ? "illegal instruction" : "signal", (uintptr_t)si->si_addr, 0);
  signal(sig, SIG_DFL); raise(sig);
}
void InstallCrashHandler(CrashFn fn) {
  g_crash = fn; struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = CrashSig; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
  for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(s, &sa, nullptr);
}

}  // namespace plat
#endif
