// Original Xbox kernel layer for the native 32-bit host. Guest pointers are host pointers (the XBE runs in this process at its own addresses).
#pragma once
#include <cstdint>
#include <cstdio>
#include "plat.h"

struct KernelImport { int ordinal; const char* name; char conv; void* fn; int argc; uint32_t ptrMask; };   // ptrMask: bit i set = argument i is a pointer into guest memory (translated by the lift bridge)
uint32_t HlePtrMask(const char* name);  // conv: s=stdcall f=fastcall c=cdecl v=data variable
extern const KernelImport g_kernelImports[];
extern const int g_kernelImportCount;

uint32_t KernelUnimplemented(int ordinal, const char* name);  // logs once per import, returns STATUS_NOT_IMPLEMENTED
void* KernelVariable(int ordinal);                            // storage for exported data variables (KeTickCount, XboxHardwareInfo...)
void KernelInit();
void Log(const char* fmt, ...);

constexpr uint32_t STATUS_SUCCESS = 0, STATUS_NOT_IMPLEMENTED = 0xC0000002u, STATUS_NO_SUCH_FILE = 0xC000000Fu, STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034u,
                   STATUS_INVALID_PARAMETER = 0xC000000Du, STATUS_NO_MEMORY = 0xC0000017u, STATUS_END_OF_FILE = 0xC0000011u, STATUS_TIMEOUT = 0x102,
                   STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003Au, STATUS_UNSUCCESSFUL = 0xC0000001u, STATUS_PENDING = 0x103;

// disc.cpp: read-only XDVDFS image
bool DiscOpen(const char* isoPath);
struct DiscEntry { uint64_t offset; uint32_t size; bool dir; };
bool DiscFind(const char* path, DiscEntry* out);   // path relative to the disc root, '\' or '/' separated, case-insensitive
bool DiscRead(uint64_t offset, void* dst, uint32_t len);
#include <vector>
#include <string>
void DiscList(const char* dirPath, std::vector<std::string>* names);  // names of the entries directly inside a disc directory

// xthread.cpp: per-thread Xbox KPCR/KTHREAD/TLS (see the comment there)
void XThreadSetTls(uint32_t tlsDirAddr);
void XThreadInit(uint32_t extraTls);
int XThreadPatch(const char* listFile);
// mmio.cpp: NV2A register window at 0xFD000000
void MmioInit();
void KernelDeliverInterrupt(uint32_t vector);  // runs the connected ISR(s) for a vector (kernel.cpp)
namespace plat { struct Shm; }
extern plat::Shm* g_ramMap;
void PrivInit();  // priv.cpp: emulate port I/O and other privileged instructions
void PrivDump();
void DisplayInit();  // display.cpp
int PatchesApply(const char* file);  // xthread.cpp
void BpInit();  // bp.cpp: XBOX_BP debug breakpoints
int DsoundHleInstall(const char* file);  // dsound_hle.cpp
void HleRegister(const char* name, void* fn);  // dsound_hle.cpp: map a symbol name from work/title/dsound_hle.txt to a host function

// Call a guest function (stdcall, up to 4 args) from host code; runs lifted code when XBOX_LIFT=1 (host/lift_rt.cpp).
uint32_t GuestCall(uint32_t fn, int n, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0);
