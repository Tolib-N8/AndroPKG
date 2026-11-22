#include "andropkg/ref_arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

namespace art {

uintptr_t gAndropkgRefBase = 0;
uintptr_t gAndropkgRefEnd = 0;

void AndropkgReserveRefArena() {
  if (gAndropkgRefBase != 0) return;
  // Prefer a 4 GB-aligned arena: a compressed reference is then just the pointer's low 32 bits
  // and decompression a single add (what nterp's assembly relies on). The kernel takes the
  // hint when that range is free; otherwise try the next 4 GB boundary.
  // kAndropkgRefArenaPreferredBase first: it is an AArch64 logical immediate, so nterp
  // decompresses with one `orr xN, xN, #base` and needs no base register.
  void* p = MAP_FAILED;
  for (uintptr_t k = kAndropkgRefArenaPreferredBase >> 32; k < 256 && p == MAP_FAILED;
       k = (k == kAndropkgRefArenaPreferredBase >> 32) ? 2 : k + 1) {
    void* hint = reinterpret_cast<void*>(k << 32);
    void* got = mmap(hint, kAndropkgRefArenaSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (got == hint) {
      p = got;
    } else if (got != MAP_FAILED) {
      munmap(got, kAndropkgRefArenaSize);
    }
  }
  if (p == MAP_FAILED) {
    p = mmap(nullptr, kAndropkgRefArenaSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  }
  if (p == MAP_FAILED) {
    perror("andropkg: cannot reserve the 4GB reference arena");
    abort();
  }
  gAndropkgRefBase = reinterpret_cast<uintptr_t>(p);
  gAndropkgRefEnd = gAndropkgRefBase + kAndropkgRefArenaSize;
  if (getenv("ANDROPKG_VERBOSE") != nullptr) {
    fprintf(stderr, "andropkg: reference arena [%#lx, %#lx)%s\n", (unsigned long)gAndropkgRefBase,
            (unsigned long)gAndropkgRefEnd, (gAndropkgRefBase & 0xffffffffu) == 0 ? " (4 GB-aligned)" : "");
  }
}

}  // namespace art
