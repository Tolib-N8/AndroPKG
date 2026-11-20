#include "andropkg/ref_arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

namespace art {

uintptr_t gAndropkgRefBase = 0;
uintptr_t gAndropkgRefEnd = 0;

void AndropkgReserveRefArena() {
  if (gAndropkgRefBase != 0) return;
  void* p = mmap(nullptr, kAndropkgRefArenaSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) {
    perror("andropkg: cannot reserve the 4GB reference arena");
    abort();
  }
  gAndropkgRefBase = reinterpret_cast<uintptr_t>(p);
  gAndropkgRefEnd = gAndropkgRefBase + kAndropkgRefArenaSize;
  if (getenv("ANDROPKG_VERBOSE") != nullptr) {
    fprintf(stderr, "andropkg: reference arena [%#lx, %#lx)\n", (unsigned long)gAndropkgRefBase,
            (unsigned long)gAndropkgRefEnd);
  }
}

}  // namespace art
