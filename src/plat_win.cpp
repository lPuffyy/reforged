// Windows implementation of plat.h: thin wrappers, same behaviour as calling Win32 directly. An Obj* is the Win32 HANDLE itself.
#ifdef _WIN32
#define _CRT_RAND_S
#include <stdlib.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include "plat.h"
#include <cstring>

namespace plat {

uint64_t TickMs() { return GetTickCount64(); }
uint64_t PerfCounter() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return (uint64_t)v.QuadPart; }
uint64_t PerfFreq() { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return (uint64_t)v.QuadPart; }
uint64_t SystemTime100ns() { FILETIME ft; GetSystemTimeAsFileTime(&ft); return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }
void SleepMs(uint32_t ms) { Sleep(ms); }
void YieldThread() { SwitchToThread(); }
void BeginHighResTimer() { timeBeginPeriod(1); }

struct ThreadCtx { ThreadFn fn; void* arg; };
static DWORD WINAPI ThreadTramp(void* p) { ThreadCtx c = *(ThreadCtx*)p; delete (ThreadCtx*)p; return c.fn(c.arg); }   // also adapts calling conventions on 32-bit
Obj* ThreadStart(ThreadFn fn, void* arg, size_t stackSize, bool suspended, uint32_t* idOut) {
  DWORD id = 0; HANDLE h = CreateThread(nullptr, stackSize, ThreadTramp, new ThreadCtx{fn, arg}, suspended ? CREATE_SUSPENDED : 0, &id);
  if (idOut) *idOut = id;
  return (Obj*)h;
}
uint32_t ThreadResume(Obj* t) { return ResumeThread((HANDLE)t); }
void ThreadExit(uint32_t code) { ExitThread(code); for (;;) {} }
uint32_t ThreadId() { return GetCurrentThreadId(); }

Obj* EventNew(bool manual, bool set) { return (Obj*)CreateEventA(nullptr, manual, set, nullptr); }
Obj* SemaphoreNew(int32_t initial, int32_t maximum) { return (Obj*)CreateSemaphoreA(nullptr, initial, maximum, nullptr); }
Obj* MutexNew(bool owned) { return (Obj*)CreateMutexA(nullptr, owned, nullptr); }
bool EventSet(Obj* o) { return SetEvent((HANDLE)o) != 0; }
bool EventReset(Obj* o) { return ResetEvent((HANDLE)o) != 0; }
bool EventPulse(Obj* o) { return PulseEvent((HANDLE)o) != 0; }
bool SemaphoreRelease(Obj* o, int32_t n, int32_t* prev) { LONG p = 0; BOOL ok = ReleaseSemaphore((HANDLE)o, n, &p); if (prev) *prev = p; return ok != 0; }
bool MutexRelease(Obj* o) { return ReleaseMutex((HANDLE)o) != 0; }
Obj* ObjDup(Obj* o) { HANDLE d = nullptr; if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)o, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) return nullptr; return (Obj*)d; }
void ObjClose(Obj* o) { if (o) CloseHandle((HANDLE)o); }
uint32_t Wait(Obj* o, uint32_t ms) { DWORD r = WaitForSingleObjectEx((HANDLE)o, ms, FALSE); return r == WAIT_OBJECT_0 ? kWaitOk : r == WAIT_TIMEOUT ? kWaitTimeout : kWaitFailed; }
uint32_t WaitMany(Obj* const* objs, uint32_t count, bool all, uint32_t ms) {
  HANDLE hs[64]; if (count > 64) count = 64; for (uint32_t i = 0; i < count; i++) hs[i] = (HANDLE)objs[i];
  DWORD r = WaitForMultipleObjectsEx(count, hs, all, ms, FALSE);
  return r < count ? r : r == WAIT_TIMEOUT ? kWaitTimeout : kWaitFailed;
}

uint32_t SignalAndWait(Obj* sig, Obj* wait, uint32_t ms) { DWORD r = SignalObjectAndWait((HANDLE)sig, (HANDLE)wait, ms, FALSE); return r == WAIT_OBJECT_0 ? kWaitOk : r == WAIT_TIMEOUT ? kWaitTimeout : kWaitFailed; }

static DWORD Prot(int p) {
  if (p & kProtExec) return (p & kProtWrite) ? PAGE_EXECUTE_READWRITE : (p & kProtRead) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
  return (p & kProtWrite) ? PAGE_READWRITE : (p & kProtRead) ? PAGE_READONLY : PAGE_NOACCESS;
}
void* VmReserve(void* at, size_t size) { return VirtualAlloc(at, size, MEM_RESERVE, PAGE_NOACCESS); }
bool VmCommit(void* at, size_t size, int prot) { return VirtualAlloc(at, size, MEM_COMMIT, Prot(prot)) != nullptr; }
bool VmProtect(void* at, size_t size, int prot) { DWORD old; return VirtualProtect(at, size, Prot(prot), &old) != 0; }
bool VmRelease(void* at, size_t size) { return size ? VirtualFree(at, size, MEM_DECOMMIT) != 0 : VirtualFree(at, 0, MEM_RELEASE) != 0; }
void* VmAlloc(void* at, size_t size, uint32_t type, uint32_t prot) { return VirtualAlloc(at, size, type, prot); }
bool VmFree(void* at, size_t size, uint32_t type) { return (type & MEM_RELEASE) ? VirtualFree(at, 0, MEM_RELEASE) != 0 : VirtualFree(at, size, type) != 0; }
bool VmProtectWin(void* at, size_t size, uint32_t prot) { DWORD old; return VirtualProtect(at, size, prot, &old) != 0; }
bool VmQuery(const void* at, VmInfo* out) {
  MEMORY_BASIC_INFORMATION mi; if (!VirtualQuery(at, &mi, sizeof mi)) return false;
  out->base = mi.BaseAddress; out->size = mi.RegionSize; out->state = mi.State == MEM_FREE ? 0 : mi.State == MEM_RESERVE ? 1 : 2;
  int p = 0; if (!(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) { p |= kProtRead; if (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) p |= kProtWrite; if (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) p |= kProtExec; }
  out->prot = out->state == 2 ? p : 0; return true;
}

Shm* ShmCreate(size_t size) { return (Shm*)CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)((uint64_t)size >> 32), (DWORD)size, nullptr); }
void* ShmMap(Shm* s, size_t offset, size_t size, void* at) { return MapViewOfFileEx((HANDLE)s, FILE_MAP_ALL_ACCESS, (DWORD)((uint64_t)offset >> 32), (DWORD)offset, size, at); }
void ShmUnmap(void* at, size_t) { UnmapViewOfFile(at); }

struct File { HANDLE h; };
File* FileOpen(const char* path, bool write, Disp d, bool* pathNotFound) {
  DWORD disp = OPEN_EXISTING;
  switch (d) { case Disp::OpenExisting: disp = OPEN_EXISTING; break; case Disp::CreateNew: disp = CREATE_NEW; break; case Disp::CreateAlways: disp = CREATE_ALWAYS; break; case Disp::OpenAlways: disp = OPEN_ALWAYS; break; case Disp::TruncateExisting: disp = TRUNCATE_EXISTING; break; }
  HANDLE h = CreateFileA(path, write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) { if (pathNotFound) *pathNotFound = GetLastError() == ERROR_PATH_NOT_FOUND; return nullptr; }
  return new File{h};
}
int64_t FileReadAt(File* f, void* buf, uint32_t len, uint64_t off) {
  OVERLAPPED ov = {}; ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32); DWORD n = 0;
  if (!ReadFile(f->h, buf, len, &n, &ov) && GetLastError() != ERROR_HANDLE_EOF) return -1;
  return n;
}
int64_t FileWriteAt(File* f, const void* buf, uint32_t len, uint64_t off) {
  OVERLAPPED ov = {}; ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32); DWORD n = 0;
  if (!WriteFile(f->h, buf, len, &n, &ov)) return -1;
  return n;
}
bool FileFlush(File* f) { return FlushFileBuffers(f->h) != 0; }
bool FileTruncate(File* f, uint64_t size) { LARGE_INTEGER p; p.QuadPart = (LONGLONG)size; return SetFilePointerEx(f->h, p, nullptr, FILE_BEGIN) && SetEndOfFile(f->h); }
uint64_t FileSize(File* f) { LARGE_INTEGER s{}; GetFileSizeEx(f->h, &s); return (uint64_t)s.QuadPart; }
void FileClose(File* f) { if (f) { CloseHandle(f->h); delete f; } }
bool PathExists(const char* path, bool* isDir) { DWORD a = GetFileAttributesA(path); if (a == INVALID_FILE_ATTRIBUTES) return false; if (isDir) *isDir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0; return true; }
bool PathStat(const char* path, StatInfo* o) {
  WIN32_FILE_ATTRIBUTE_DATA ad; if (!GetFileAttributesExA(path, GetFileExInfoStandard, &ad)) return false;
  auto ft = [](const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; };
  o->size = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow; o->isDir = (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  o->created = ft(ad.ftCreationTime); o->accessed = ft(ad.ftLastAccessTime); o->written = ft(ad.ftLastWriteTime); return true;
}
bool MakeDir(const char* path) { return CreateDirectoryA(path, nullptr) != 0; }
bool RemoveDir(const char* path) { return RemoveDirectoryA(path) != 0; }
bool RemoveFile(const char* path) { return DeleteFileA(path) != 0; }
bool RenameFile(const char* from, const char* to) { return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0; }
std::vector<std::string> ListDir(const char* dir, const char* pattern) {
  std::vector<std::string> out; WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA((std::string(dir) + "/" + pattern).c_str(), &fd);
  if (fh != INVALID_HANDLE_VALUE) { do { if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) out.push_back(fd.cFileName); } while (FindNextFileA(fh, &fd)); FindClose(fh); }
  return out;
}

void* LoadLib(const char* name) { return (void*)LoadLibraryA(name); }
void* LibSym(void* lib, const char* name) { return (void*)GetProcAddress((HMODULE)lib, name); }

struct AudioCtx { uint32_t rate, frames, buffers; AudioFn fn; void* user; };
static uint32_t AudioThread(void* p) {
  AudioCtx c = *(AudioCtx*)p; delete (AudioCtx*)p;
  WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, c.rate, c.rate * 4, 4, 16, 0}; HWAVEOUT hw = nullptr;
  HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (waveOutOpen(&hw, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) return 0;
  std::vector<int16_t> bufs((size_t)c.buffers * c.frames * 2); std::vector<WAVEHDR> hdr(c.buffers);
  for (uint32_t i = 0; i < c.buffers; i++) { memset(&hdr[i], 0, sizeof(WAVEHDR)); hdr[i].lpData = (LPSTR)&bufs[(size_t)i * c.frames * 2]; hdr[i].dwBufferLength = c.frames * 4; hdr[i].dwFlags = WHDR_DONE; }
  for (;;) {
    bool any = false;
    for (uint32_t i = 0; i < c.buffers; i++) if (hdr[i].dwFlags & WHDR_DONE) {
      c.fn((int16_t*)hdr[i].lpData, c.user);
      if (hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(hw, &hdr[i], sizeof hdr[i]);
      hdr[i].dwFlags = 0; waveOutPrepareHeader(hw, &hdr[i], sizeof hdr[i]); waveOutWrite(hw, &hdr[i], sizeof hdr[i]); any = true;
    }
    if (!any) WaitForSingleObject(ev, 20);
  }
}
bool AudioOutStart(uint32_t rate, uint32_t frames, uint32_t buffers, AudioFn fn, void* user) { ThreadSpawn(AudioThread, new AudioCtx{rate, frames, buffers, fn, user}); return true; }

void SetEnv(const char* name, const char* value) { SetEnvironmentVariableA(name, value); }
bool SpawnSelf(const std::vector<std::string>& args) {
  char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH); std::string cmd = std::string("\"") + exe + "\"";
  for (auto& a : args) cmd += " \"" + a + "\"";
  STARTUPINFOA si{sizeof si}; PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return false;
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true;
}

uint32_t RandomU32() { unsigned v = 0; rand_s(&v); return v; }

static CrashFn g_crash;
static LONG WINAPI CrashTramp(EXCEPTION_POINTERS* e) {
  if (g_crash) g_crash(e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? "access violation" : "exception", e->ExceptionRecord->ExceptionInformation[1], (uintptr_t)e->ExceptionRecord->ExceptionAddress);
  return EXCEPTION_CONTINUE_SEARCH;
}
void InstallCrashHandler(CrashFn fn) { g_crash = fn; SetUnhandledExceptionFilter(CrashTramp); }

}  // namespace plat
#endif
