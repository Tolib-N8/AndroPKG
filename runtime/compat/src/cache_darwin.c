// andropkg: compiler-rt's __clear_cache isn't exported to iOS apps; use the libkern equivalent.
#include <libkern/OSCacheControl.h>
#include <stddef.h>

void __clear_cache(void* begin, void* end) {
  sys_icache_invalidate(begin, (size_t)((char*)end - (char*)begin));
}
