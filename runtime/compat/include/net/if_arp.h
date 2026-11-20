// andropkg: <net/if_arp.h> exists in the macOS SDK but not the iOS one; libcore only needs the
// ARPHRD_* hardware types for android.system.OsConstants.
#pragma once
#if __has_include_next(<net/if_arp.h>)
#include_next <net/if_arp.h>
#else
#define ARPHRD_ETHER 1
#define ARPHRD_IEEE802 6
#define ARPHRD_FRELAY 15
#define ARPHRD_IEEE1394 24
#define ARPHRD_LOOPBACK 772
#endif
