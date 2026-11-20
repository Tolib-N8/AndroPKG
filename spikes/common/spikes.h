// M0 feasibility spikes, shared by the macOS CLI harness and the iOS spike app.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int passed, failed, info;
} spike_summary;

/// Output sink. Each result is one line: "[SPIKE] PASS|FAIL|INFO <name>: <detail>".
typedef void (*spike_log_fn)(const char *line);

/// (a) TPIDR_EL0 preserved per thread across context switches (bionic TLS carrier).
/// (c) x18 behaviour (informational; Android and Darwin both reserve it).
/// (d) SIGSEGV handler can rewrite PC and resume (ART implicit null checks).
void spike_run_cpu(spike_log_fn log, spike_summary *sum);

/// (b) Load a converter-wrapped Android ELF (spikes/testlib) through the andropkg loader
/// and exercise relocations, init_array and patched svc syscalls.
void spike_run_elf(const char *wrapped_dylib, const char *label, spike_log_fn log, spike_summary *sum);

#ifdef __cplusplus
}
#endif
