// andropkg: prctl(2) constants from Linux UAPI; prctl() itself fails with EINVAL on Darwin
// except PR_SET_NAME/PR_GET_NAME (mapped to pthread names).
#pragma once
#include <linux/prctl.h>
#ifdef __cplusplus
extern "C" {
#endif
int prctl(int option, ...);
#ifdef __cplusplus
}
#endif
