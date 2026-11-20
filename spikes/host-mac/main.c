// macOS harness for the M0 spikes: same checks as the iOS app, fast iteration loop.
#include "spikes.h"
#include <stdio.h>

static void log_line(const char *line) { puts(line); fflush(stdout); }

int main(int argc, char **argv) {
    spike_summary sum = {0};
    spike_run_cpu(log_line, &sum);
    for (int i = 1; i + 1 < argc; i += 2) spike_run_elf(argv[i + 1], argv[i], log_line, &sum);
    printf("[SPIKE] SUMMARY passed=%d failed=%d info=%d\n", sum.passed, sum.failed, sum.info);
    return sum.failed ? 1 : 0;
}
