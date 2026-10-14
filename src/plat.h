// Platform layer: everything the runtime needs from the host OS that is not plain C++17. plat_win.cpp wraps Win32 (identical behaviour to
// calling it directly); plat_posix.cpp implements the same API on Linux/macOS/iOS. The runtime (kernel HLE, memory, translated-code
// support) must only use this header for OS services. GPU, window, audio-out and input stay platform specific.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Guest-visible numeric flags (the original system uses the same values as Win32); names are guarded so windows.h can coexist.
#ifndef MEM_COMMIT
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define MEM_PRIVATE 0x00020000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#endif

// Calling conventions only matter for the 32-bit native-fallback build (MSVC x86); everywhere else they vanish.
#if defined(_MSC_VER)
#include <intrin.h>
#define PLAT_STDCALL __stdcall
#define PLAT_FASTCALL __fastcall
#define PLAT_CDECL __cdecl
#define PLAT_RETADDR() _ReturnAddress()
#else
#include <strings.h>
#define PLAT_STDCALL
#define PLAT_FASTCALL
#define PLAT_CDECL
#define PLAT_RETADDR() __builtin_return_address(0)
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif

namespace plat {

// ---- time ----
uint64_t TickMs();                 // monotonic milliseconds
uint64_t PerfCounter();            // high resolution monotonic counter
uint64_t PerfFreq();               // PerfCounter ticks per second
uint64_t SystemTime100ns();        // 100 ns units since 1601-01-01 UTC (FILETIME)
void SleepMs(uint32_t ms);         // 0 yields the processor
void YieldThread();
void BeginHighResTimer();          // ask the OS for 1 ms timer resolution (Windows); no-op elsewhere

// ---- bit counting ----
#if defined(_MSC_VER)
inline int PopCount32(uint32_t v) { return (int)__popcnt(v); }
#else
inline int PopCount32(uint32_t v) { return __builtin_popcount(v); }
#endif

// ---- atomics (return the previous value, like the Interlocked* family) ----
#if defined(_MSC_VER)
inline int32_t AtomicAdd(volatile int32_t* p, int32_t v) { return _InterlockedExchangeAdd((volatile long*)p, v); }
inline int32_t AtomicExchange(volatile int32_t* p, int32_t v) { return _InterlockedExchange((volatile long*)p, v); }
inline int32_t AtomicAnd(volatile int32_t* p, int32_t v) { return _InterlockedAnd((volatile long*)p, v); }
inline int32_t AtomicOr(volatile int32_t* p, int32_t v) { return _InterlockedOr((volatile long*)p, v); }
inline int32_t AtomicXor(volatile int32_t* p, int32_t v) { return _InterlockedXor((volatile long*)p, v); }
inline int32_t AtomicCas(volatile int32_t* p, int32_t expect, int32_t desired) { return _InterlockedCompareExchange((volatile long*)p, desired, expect); }
#else
inline int32_t AtomicAdd(volatile int32_t* p, int32_t v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
inline int32_t AtomicExchange(volatile int32_t* p, int32_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline int32_t AtomicAnd(volatile int32_t* p, int32_t v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
inline int32_t AtomicOr(volatile int32_t* p, int32_t v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
inline int32_t AtomicXor(volatile int32_t* p, int32_t v) { return __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST); }
inline int32_t AtomicCas(volatile int32_t* p, int32_t expect, int32_t desired) { __atomic_compare_exchange_n(p, &expect, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return expect; }
#endif

// ---- threads and waitable kernel objects ----
// One opaque type for everything a guest can wait on (events, semaphores, mutexes, threads).
struct Obj;
typedef uint32_t (*ThreadFn)(void*);
Obj* ThreadStart(ThreadFn fn, void* arg, size_t stackSize, bool suspended, uint32_t* idOut);   // the returned object is signalled when the thread exits
uint32_t ThreadResume(Obj* thread);                                                            // returns the previous suspend count
[[noreturn]] void ThreadExit(uint32_t code);
uint32_t ThreadId();
inline void ThreadSpawn(ThreadFn fn, void* arg = nullptr, size_t stackSize = 0);               // fire and forget (defined below)

Obj* EventNew(bool manualReset, bool initiallySet);
Obj* SemaphoreNew(int32_t initial, int32_t maximum);
Obj* MutexNew(bool initiallyOwned);
bool EventSet(Obj*);
bool EventReset(Obj*);
bool EventPulse(Obj*);              // release every current waiter of a manual-reset event, leaving it unsignalled
bool SemaphoreRelease(Obj*, int32_t count, int32_t* previous);
bool MutexRelease(Obj*);
Obj* ObjDup(Obj*);                 // a second reference to the same object
void ObjClose(Obj*);               // drops one reference

enum : uint32_t { kWaitOk = 0, kWaitTimeout = 0x102, kWaitFailed = 0xFFFFFFFFu, kInfinite = 0xFFFFFFFFu };
uint32_t Wait(Obj*, uint32_t ms);                                          // kWaitOk / kWaitTimeout
uint32_t WaitMany(Obj* const* objs, uint32_t count, bool waitAll, uint32_t ms);
uint32_t SignalAndWait(Obj* signal, Obj* wait, uint32_t ms);              // signal one object (event set / semaphore release / mutex release) then wait on another   // index of the signalled object, or kWaitTimeout

// ---- virtual memory ----
enum : int { kProtNone = 0, kProtRead = 1, kProtWrite = 2, kProtExec = 4, kProtRW = 3, kProtRWX = 7 };
void* VmReserve(void* at, size_t size);          // reserve address space (at == nullptr: anywhere); no access until committed
bool VmCommit(void* at, size_t size, int prot);  // back reserved pages with memory (zero filled)
bool VmProtect(void* at, size_t size, int prot);
bool VmRelease(void* at, size_t size);           // return pages to the OS (size 0: the whole reservation that starts at `at`)
struct VmInfo { void* base; size_t size; int state; int prot; };   // state: 0 free, 1 reserved, 2 committed
bool VmQuery(const void* at, VmInfo* out);
// The same operations with Win32-style numeric flags (MEM_* type, PAGE_* protection), which is what the guest passes.
void* VmAlloc(void* at, size_t size, uint32_t type, uint32_t protWin);   // VirtualAlloc semantics: null on failure; a fixed address that is already taken fails
bool VmFree(void* at, size_t size, uint32_t type);                       // MEM_RELEASE (size ignored, whole reservation) or MEM_DECOMMIT
bool VmProtectWin(void* at, size_t size, uint32_t protWin);

// Shared backing store that can be mapped at several addresses (hardware windows that alias RAM).
struct Shm;
Shm* ShmCreate(size_t size);
void* ShmMap(Shm*, size_t offset, size_t size, void* at);   // map [offset, offset+size) at a fixed address (replacing what is there); nullptr on failure
void ShmUnmap(void* at, size_t size);

// ---- files and directories (paths are UTF-8, '/' separators) ----
struct File;
enum class Disp { OpenExisting, CreateNew, CreateAlways, OpenAlways, TruncateExisting };
File* FileOpen(const char* path, bool write, Disp disp, bool* pathNotFound);
int64_t FileReadAt(File*, void* buf, uint32_t len, uint64_t offset);    // bytes read, 0 at end of file, -1 on error
int64_t FileWriteAt(File*, const void* buf, uint32_t len, uint64_t offset);
bool FileFlush(File*);
bool FileTruncate(File*, uint64_t size);
uint64_t FileSize(File*);
void FileClose(File*);
struct StatInfo { uint64_t size; bool isDir; uint64_t created, accessed, written; };   // times in FILETIME units (100 ns since 1601)
bool PathExists(const char* path, bool* isDir);
bool PathStat(const char* path, StatInfo* out);
bool MakeDir(const char* path);
bool RemoveDir(const char* path);
bool RemoveFile(const char* path);
bool RenameFile(const char* from, const char* to);                      // replaces an existing target
std::vector<std::string> ListDir(const char* dir, const char* pattern); // entry names (no . or ..) matching a case-insensitive * ? pattern

// ---- dynamic libraries ----
void* LoadLib(const char* name);
void* LibSym(void* lib, const char* name);

// ---- audio output ----
// Pull model: the backend calls fn(out, user) from its own thread whenever it needs `frames` stereo frames of 16-bit PCM (interleaved).
// The Windows backend plays through the default device; the portable backend is a real-time paced silent sink until an app shell provides a device.
typedef void (*AudioFn)(int16_t* out, void* user);
bool AudioOutStart(uint32_t rate, uint32_t frames, uint32_t buffers, AudioFn fn, void* user);

// ---- process ----
void SetEnv(const char* name, const char* value);
bool SpawnSelf(const std::vector<std::string>& args);   // start another instance of this program with these arguments (false where processes cannot be spawned, e.g. iOS)

// ---- diagnostics ----
typedef void (*CrashFn)(const char* what, uintptr_t address, uintptr_t ip);
void InstallCrashHandler(CrashFn fn);    // called on an access violation / illegal instruction; the process then terminates

inline void ThreadSpawn(ThreadFn fn, void* arg, size_t stackSize) { ObjClose(ThreadStart(fn, arg, stackSize, false, nullptr)); }

}  // namespace plat
