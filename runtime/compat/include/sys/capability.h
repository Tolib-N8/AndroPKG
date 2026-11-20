// andropkg: Linux capabilities don't exist on Darwin; capget() reports no capabilities.
#pragma once
#include <linux/capability.h>
#ifdef __cplusplus
extern "C" {
#endif
int capget(cap_user_header_t hdrp, cap_user_data_t datap);
int capset(cap_user_header_t hdrp, const cap_user_data_t datap);
#ifdef __cplusplus
}
#endif
