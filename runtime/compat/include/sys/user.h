// andropkg: <sys/user.h> exists in the macOS SDK but not the iOS one (AOSP wants PAGE_SIZE).
#pragma once
#if __has_include_next(<sys/user.h>)
#include_next <sys/user.h>
#else
#include <mach/vm_param.h>
#endif
