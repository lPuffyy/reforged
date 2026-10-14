"""Generate src/gen/kernel_stubs.cpp from tools/kernel_imports.txt: a default logging stub for every import that src/kernel*.cpp
does not implement itself (a K_<Name> definition), plus the ordinal table used to patch the XBE kernel thunk table.
usage: python -I tools/gen_kernel.py"""
import re, os, glob
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# stdcall argument counts (32-bit words popped by the callee); fastcall/cdecl/variables are handled separately.
ARGC = dict(XcBlockCrypt=5, KeGetCurrentThread=0, KeInitializeEvent=3, KeInitializeSemaphore=3, KeReleaseSemaphore=4, Unknown320=2, HalReadSMCTrayState=2, ExAcquireReadWriteLockExclusive=1, ExAcquireReadWriteLockShared=1, ExInitializeReadWriteLock=1, ExReadWriteRefurbInfo=3, ExReleaseReadWriteLock=1, ExSaveNonVolatileSetting=4, FscSetCacheSize=1, Unknown91=1, Unknown124=2, NtClearEvent=1, NtSignalAndWaitForSingleObjectEx=5, RtlUpcaseUnicodeChar=1, HalEnableSecureTrayEject=0,
 AvGetSavedDataAddress=0, AvSendTVEncoderOption=4, AvSetDisplayMode=6, AvSetSavedDataAddress=1, ExAllocatePoolWithTag=2, ExFreePool=1,
 ExQueryPoolBlockSize=1, ExQueryNonVolatileSetting=5, HalGetInterruptVector=2, HalReadWritePCISpace=6, HalRegisterShutdownNotification=2,
 HalReturnToFirmware=1, IoBuildSynchronousFsdRequest=7, IoCreateDevice=6, IoCreateSymbolicLink=2, IoDeleteSymbolicLink=1, IoInvalidDeviceRequest=2,
 IoStartNextPacket=1, IoStartPacket=3, IoSynchronousDeviceIoControlRequest=8, IoSynchronousFsdRequest=5, KeBugCheck=1, KeCancelTimer=1,
 KeConnectInterrupt=1, KeDelayExecutionThread=3, KeDisconnectInterrupt=1, KeInitializeDpc=3, KeInitializeInterrupt=7, KeInitializeTimerEx=2,
 KeInsertQueueDpc=3, KeQueryPerformanceCounter=0, KeQueryPerformanceFrequency=0, KeQuerySystemTime=1, KeRaiseIrqlToDpcLevel=0, KeRemoveQueueDpc=1,
 KeRestoreFloatingPointState=1, KeSaveFloatingPointState=1, KeSetBasePriorityThread=2, KeSetEvent=3, KeSetTimer=4, KeSetTimerEx=5,
 KeStallExecutionProcessor=1, KeSynchronizeExecution=3, KeWaitForMultipleObjects=8, KeWaitForSingleObject=5, MmAllocateContiguousMemory=1,
 MmAllocateContiguousMemoryEx=5, MmAllocateSystemMemory=3, MmClaimGpuInstanceMemory=2, MmCreateKernelStack=2, MmDeleteKernelStack=2,
 MmFreeContiguousMemory=1, MmFreeSystemMemory=2, MmGetPhysicalAddress=1, MmLockUnlockBufferPages=3, MmLockUnlockPhysicalPage=2,
 MmPersistContiguousMemory=3, MmQueryAddressProtect=1, MmQueryAllocationSize=1, MmQueryStatistics=1, MmSetAddressProtect=3,
 NtAllocateVirtualMemory=5, NtClose=1, NtCreateEvent=4, NtCreateFile=9, NtCreateMutant=3, NtCreateSemaphore=4, NtDeleteFile=1,
 NtDeviceIoControlFile=10, NtDuplicateObject=3, NtFlushBuffersFile=2, NtFreeVirtualMemory=3, NtFsControlFile=10, NtOpenFile=6,
 NtOpenSymbolicLinkObject=2, NtQueryDirectoryFile=10, NtQueryFullAttributesFile=2, NtQueryInformationFile=5, NtQuerySymbolicLinkObject=3,
 NtQueryVirtualMemory=2, NtQueryVolumeInformationFile=5, NtReadFile=8, NtReleaseMutant=2, NtReleaseSemaphore=3, NtResumeThread=2, NtSetEvent=2,
 NtSetInformationFile=5, NtSetSystemTime=2, NtWaitForSingleObject=3, NtWaitForSingleObjectEx=4, NtWaitForMultipleObjectsEx=6, NtWriteFile=8,
 NtYieldExecution=0, ObReferenceObjectByHandle=3, ObReferenceObjectByName=5, PhyGetLinkState=1, PhyInitialize=2, PsCreateSystemThreadEx=10,
 PsTerminateSystemThread=1, RtlAnsiStringToUnicodeString=3, RtlCompareMemoryUlong=3, RtlEnterCriticalSection=1, RtlEqualString=3,
 RtlInitAnsiString=2, RtlInitializeCriticalSection=1, RtlLeaveCriticalSection=1, RtlNtStatusToDosError=1, RtlRaiseException=1,
 RtlTimeFieldsToTime=2, RtlTimeToTimeFields=2, RtlUnicodeStringToAnsiString=3, RtlUnwind=4, XeLoadSection=1, XeUnloadSection=1, XcSHAInit=1,
 XcSHAUpdate=3, XcSHAFinal=2, XcRC4Key=3, XcRC4Crypt=3, XcHMAC=7, XcPKGetKeyLen=1, XcVerifyPKCS1Signature=3, XcModExp=5, XcDESKeyParity=2,
 XcKeyTable=3, XcBlockCryptCBC=7, HalIsResetOrShutdownPending=0, IoMarkIrpMustComplete=1, HalInitiateShutdown=0)
FASTARGS = dict(InterlockedDecrement=1, InterlockedIncrement=1, InterlockedExchange=2, IofCallDriver=2, IofCompleteRequest=2, KfRaiseIrql=1, KfLowerIrql=1, ObfDereferenceObject=1)
imports = []
for line in open(os.path.join(ROOT, 'tools', 'kernel_imports.txt')):
    if line.startswith('#') or not line.strip(): continue
    o, n, c = line.split(); imports.append((int(o), n, c))
src = ''
for fn in glob.glob(os.path.join(ROOT, 'src', '*.cpp')):
    src += open(fn, encoding='utf-8').read()
def split_params(txt):
    out=[]; depth=0; cur=''
    for ch in txt:
        if ch in '(<': depth+=1
        if ch in ')>': depth-=1
        if ch==',' and depth==0: out.append(cur); cur=''
        else: cur+=ch
    if cur.strip(): out.append(cur)
    return out
def ptr_mask(params): return sum(1<<i for i,pp in enumerate(params) if '*' in pp)
KMASK = {}; HMASK = {}
for m in re.finditer(r'^(KFN|KFF|H)\((\w+)(?:,([^\n]*?))?\)\s*\{', src, re.M):
    kind,name,params=m.group(1),m.group(2),m.group(3) or ''
    pl=split_params(params); mask=ptr_mask(pl)
    (HMASK if kind=='H' else KMASK)[name]=mask
implemented = set(re.findall(r'\bK_(\w+)\s*\(', src)) | set(re.findall(r'\bKF[NF]\((\w+)', src))
out = ['// GENERATED by tools/gen_kernel.py - do not edit', '#include "kernel.h"', '#include "plat.h"', '#include <cstring>', '']
for o, n, c in imports:
    if c == 'v': continue
    if n in implemented: 
        if c == 'c': out.append(f'extern "C" uint32_t PLAT_CDECL K_{n}(const char*, ...);')
        elif n in ('KeQueryPerformanceCounter', 'KeQueryPerformanceFrequency'): out.append(f'extern "C" uint64_t PLAT_STDCALL K_{n}(void);')
        else: out.append(f'extern "C" uint32_t {"PLAT_FASTCALL" if c=="f" else "PLAT_STDCALL"} K_{n}({", ".join(["uint32_t"]*(FASTARGS.get(n,0) if c=="f" else ARGC.get(n,0))) or "void"});')
        continue
    if c == 'c':
        out.append(f'extern "C" uint32_t PLAT_CDECL K_{n}(const char* fmt, ...) {{ return KernelUnimplemented({o}, "{n}"); }}'); continue
    if c == 'f':
        args = ', '.join(f'uint32_t a{i}' for i in range(FASTARGS[n])); conv = 'PLAT_FASTCALL'
    elif c == 'c':
        args = ''; conv = 'PLAT_CDECL'
    else:
        assert n in ARGC, n
        args = ', '.join(f'uint32_t a{i}' for i in range(ARGC[n])); conv = 'PLAT_STDCALL'
    out.append(f'extern "C" uint32_t {conv} K_{n}({args or "void"}) {{ return KernelUnimplemented({o}, "{n}"); }}')
out.append('')
out.append('const KernelImport g_kernelImports[] = {')
for o, n, c in imports:
    ac = 0 if c in ('v', 'c') else (FASTARGS.get(n, 0) if c == 'f' else ARGC.get(n, 0))
    out.append(f'  {{ {o}, "{n}", \'{c}\', {"nullptr" if c=="v" else "(void*)K_"+n}, {ac}, {KMASK.get(n, 0)}u }},')
out.append('};')
out.append('uint32_t HlePtrMask(const char* name) {')
for hn,hm in sorted(HMASK.items()): out.append(f'  if (!strcmp(name, "{hn}")) return {hm}u;')
out.append('  return 0;')
out.append('}')
out.append(f'const int g_kernelImportCount = {len(imports)};')
os.makedirs(os.path.join(ROOT, 'src', 'gen'), exist_ok=True)
open(os.path.join(ROOT, 'src', 'gen', 'kernel_stubs.cpp'), 'w').write('\n'.join(out) + '\n')
print(len(imports), 'imports,', len([1 for _, n, c in imports if n in implemented]), 'implemented')
