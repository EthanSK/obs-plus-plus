#!/usr/bin/env python3
"""Exercise the actual macOS CPU sampler across thread exit and query failures."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / "libobs/util/platform-cocoa.m").read_text()
sampler = source.split("struct os_cpu_usage_info {", 1)[1].split("os_performance_token_t *os_request_high_performance", 1)[0]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/resource.h>
#include <mach/mach.h>
typedef struct os_cpu_usage_info os_cpu_usage_info_t;
#define bmalloc malloc
#define bfree free
static int sample;
static bool fail_query;
static const int64_t cpu_times[] = {1000000, 1200000, 1300000, 1400000};
static uint64_t os_gettime_ns(void) { return (uint64_t)(++sample) * 1000000000ULL; }
static int test_getrusage(int who, struct rusage *usage) {
    assert(who == RUSAGE_SELF);
    if (fail_query) return -1;
    *usage = (struct rusage){0};
    usage->ru_utime.tv_sec = cpu_times[sample] / 1000000;
    usage->ru_utime.tv_usec = cpu_times[sample] % 1000000;
    return 0;
}
static kern_return_t test_task_info(mach_port_t task, task_flavor_t flavor, task_info_t data,
                                    mach_msg_type_number_t *count) {
    (void)task; (void)count;
    if (fail_query) return KERN_FAILURE;
    if (flavor == TASK_THREAD_TIMES_INFO) {
        struct task_thread_times_info *times = (void *)data;
        *times = (struct task_thread_times_info){0};
        if (sample < 2) { times->user_time.seconds = cpu_times[sample] / 1000000;
                         times->user_time.microseconds = cpu_times[sample] % 1000000; }
    } else {
        assert(flavor == TASK_BASIC_INFO_64);
        struct task_basic_info_64 *times = (void *)data;
        *times = (struct task_basic_info_64){0};
        if (sample > 0) { times->user_time.seconds = cpu_times[sample] / 1000000;
                         times->user_time.microseconds = cpu_times[sample] % 1000000; }
    }
    return KERN_SUCCESS;
}
#define getrusage test_getrusage
#define task_info test_task_info
'''
fixture += "struct os_cpu_usage_info {" + sampler
fixture += r'''
int main(void) {
    os_cpu_usage_info_t *info = os_cpu_usage_info_start();
    assert(info); info->core_count = 1;
    assert(os_cpu_usage_info_query(info) == 20.0);
    assert(os_cpu_usage_info_query(info) == 10.0);
    puts("PASS: whole-task CPU sampling avoids the reproduced live/dead-thread double-count transition");
    int64_t cpu_baseline = info->last_sys_time;
    fail_query = true;
    assert(os_cpu_usage_info_query(info) == 0.0);
    assert(info->last_sys_time == cpu_baseline);
    assert(os_cpu_usage_info_start() == NULL);
    os_cpu_usage_info_destroy(info);
    puts("PASS: query failure preserves the baseline and startup failure returns no sampler");
}
'''

with tempfile.TemporaryDirectory(prefix="obs-cpu-test-") as directory:
    harness = Path(directory) / "cpu.m"
    binary = Path(directory) / "cpu"
    harness.write_text(fixture)
    subprocess.run(["xcrun", "clang", "-Wno-unused-function", str(harness), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
