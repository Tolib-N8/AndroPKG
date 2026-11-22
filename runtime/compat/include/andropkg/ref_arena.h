// andropkg: base-relative compressed heap references.
//
// ART stores object references as 32-bit values and relies on the Java heap living below
// 4 GB. Darwin (iOS and arm64 macOS) maps nothing below 4 GB and refuses executables with a
// smaller __PAGEZERO, so andropkg reserves one 4 GB "reference arena" at startup and makes
// ART's "low 4 GB" mean [base, base + 4 GB):
//
//     compressed ref = ptr - base      (0 <-> nullptr; the first 64 KB are never handed out)
//
// Before the arena is reserved base == 0, i.e. refs == pointers (stock ART behaviour).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace art {

extern uintptr_t gAndropkgRefBase;
extern uintptr_t gAndropkgRefEnd;

constexpr size_t kAndropkgRefArenaSize = size_t{4} << 30;
constexpr size_t kAndropkgRefArenaGuard = 64 * 1024;
// Where the arena goes when the address space allows (it does on iOS 27 and macOS): 4 GB
// aligned, so ref == low 32 bits of the pointer, and encodable as an AArch64 logical immediate.
constexpr uintptr_t kAndropkgRefArenaPreferredBase = uintptr_t{0x70} << 32;

/// Reserves the arena (PROT_NONE). Idempotent; aborts if the OS refuses.
void AndropkgReserveRefArena();

inline bool AndropkgInRefArena(uintptr_t p, size_t len) {
  return gAndropkgRefBase != 0 && p >= gAndropkgRefBase && p + len <= gAndropkgRefEnd;
}

inline uint32_t AndropkgCompressRef(uintptr_t p) {
  return p == 0 ? 0u : static_cast<uint32_t>(p - gAndropkgRefBase);
}

inline uintptr_t AndropkgDecompressRef(uint32_t r) {
  return r == 0 ? uintptr_t{0} : gAndropkgRefBase + r;
}

}  // namespace art
