#pragma once
#include <cstdint>
#include <cstddef>
struct XbeInfo {
  uint32_t base, entry, kernelThunk, stackCommit, tlsAddr, certAddr;
  uint32_t imageEnd;
  uint32_t titleId;
};
// Maps default.xbe at its own base address (0x10000 for a retail title). Returns false with a message in err.
bool XbeLoad(const char* path, XbeInfo* info, char* err, size_t errLen);
