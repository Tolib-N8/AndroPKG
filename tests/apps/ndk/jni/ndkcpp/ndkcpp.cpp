// C++ NDK library on libc++_shared: exceptions, threads, thread_local, static init.
#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Registry {
    std::vector<std::string> names;
    Registry() { names.push_back("static-init"); }
};
Registry g_registry;  // runs from init_array
thread_local int t_counter = 0;

int may_throw(int n) {
    if (n > 3) throw std::runtime_error("too big: " + std::to_string(n));
    return n * 2;
}
}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_dev_andropkg_test_ndk_MainActivity_cpp(JNIEnv* env, jclass, jint n) {
    std::string caught;
    try {
        may_throw(n);
    } catch (const std::exception& e) {
        caught = e.what();
    }
    std::atomic<int> total{0};
    std::mutex m;
    std::vector<int> seen;
    std::vector<std::thread> threads;
    for (int i = 1; i <= n; i++) {
        threads.emplace_back([&, i] {
            for (int k = 0; k < i; k++) t_counter++;
            total += t_counter;
            std::lock_guard<std::mutex> lock(m);
            seen.push_back(t_counter);
        });
    }
    for (auto& t : threads) t.join();
    std::vector<int> v(n);
    std::iota(v.begin(), v.end(), 1);
    std::string out = g_registry.names[0] + " caught=[" + caught + "] threads=" + std::to_string(total.load()) +
                      " seen=" + std::to_string(seen.size()) + " sum=" + std::to_string(std::accumulate(v.begin(), v.end(), 0));
    __android_log_print(ANDROID_LOG_INFO, "ndkcpp", "%s", out.c_str());
    return env->NewStringUTF(out.c_str());
}
