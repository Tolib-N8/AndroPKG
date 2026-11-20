// andropkg: bionic headers include <sys/cdefs.h> and use a few bionic-only macros.
#pragma once
#include_next <sys/cdefs.h>
#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(x)
#endif
#ifndef __BIONIC_AVAILABILITY_GUARD
#define __BIONIC_AVAILABILITY_GUARD(x) 1
#endif
