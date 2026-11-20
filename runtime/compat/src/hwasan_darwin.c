// andropkg: ART references these HWASan hooks weakly and calls them when non-null. Mach-O
// links need a definition, so provide the semantics of a non-HWASan build.
#include <stdint.h>

void __hwasan_handle_longjmp(const void* sp_dst) { (void)sp_dst; }
void* __hwasan_tag_pointer(const volatile void* p, unsigned char tag) {
  (void)tag;
  return (void*)(uintptr_t)p;
}
