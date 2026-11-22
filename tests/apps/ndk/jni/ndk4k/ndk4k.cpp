// Linked with -z max-page-size=4096 (PAGE4K marker), as older NDKs did: code and data share a
// 16 KB page, so the converter moves the data (converter Layout4K). Exercises every kind of
// reference from code into the moved data: ADRP+ADD/LDR to globals, GOT (libc++ calls), relocated
// pointer tables, C++ exceptions caught by type (LSDA type table) and by base class.
#include <jni.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
int g_counter = 7;                                  // .data
int g_table[64] = {1, 2, 3};                        // .data
int g_bss[1024];                                    // .bss
const char* const g_names[] = {"alpha", "beta"};    // .data.rel.ro (relocated)
int twice(int x) { return 2 * x; }
int thrice(int x) { return 3 * x; }
int (*const g_fns[])(int) = {twice, thrice};        // .data.rel.ro (relocated)

struct Custom {
    int code;
};

__attribute__((noinline)) void raise(int n) {
    if (n == 1) throw Custom{42};
    if (n == 2) throw std::out_of_range("range");
}

std::string catches(int n) {
    try {
        raise(n);
    } catch (const Custom& c) {
        return "Custom(" + std::to_string(c.code) + ")";
    } catch (const std::logic_error& e) {
        return std::string("logic_error(") + e.what() + ")";
    }
    return "none";
}
}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_dev_andropkg_test_ndk_MainActivity_page4k(JNIEnv* env, jclass) {
    g_counter += 1;
    g_table[63] = g_table[0] + g_table[1] + g_table[2];
    for (int i = 0; i < 1024; i++) g_bss[i] = i;
    long bss = 0;
    for (int i = 0; i < 1024; i++) bss += g_bss[i];
    char buf[256];
    snprintf(buf, sizeof buf, "counter=%d table=%d bss=%ld names=%s,%s fns=%d,%d catch=%s,%s", g_counter,
             g_table[63], bss, g_names[0], g_names[1], g_fns[0](5), g_fns[1](5), catches(1).c_str(),
             catches(2).c_str());
    return env->NewStringUTF(buf);
}
