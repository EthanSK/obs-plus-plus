#!/usr/bin/env python3
"""Exercise the actual macOS semaphore timeout implementation with native Mach semaphores."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / "libobs/util/threading-posix.c").read_text()


def function(name):
    start = re.search(r"^int " + name + r"\(", source, re.MULTILINE).start()
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


fixture = r'''
#include <assert.h>
#include <errno.h>
#include <mach/semaphore.h>
#include <mach/task.h>
#include <mach/mach_init.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
struct os_sem_data { semaphore_t sem; task_t task; };
typedef struct os_sem_data os_sem_t;
#define bzalloc(size) calloc(1, size)
'''
fixture += function("os_sem_init") + function("os_sem_post") + function("os_sem_timedwait")
fixture += r'''
static void *post_later(void *data) { usleep(20000); assert(os_sem_post(data) == 0); return NULL; }
static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
int main(void) {
    os_sem_t *sem;
    assert(os_sem_init(&sem, 0) == 0);
    assert(os_sem_timedwait(NULL, 1) == EINVAL);
    uint64_t start = now_ns();
    assert(os_sem_timedwait(sem, 25) == ETIMEDOUT);
    assert(now_ns() - start >= 20000000ULL);
    assert(os_sem_post(sem) == 0 && os_sem_post(sem) == 0);
    assert(os_sem_timedwait(sem, 0) == 0 && os_sem_timedwait(sem, 0) == 0);
    assert(os_sem_timedwait(sem, 0) == ETIMEDOUT);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, post_later, sem) == 0);
    assert(os_sem_timedwait(sem, 500) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(semaphore_destroy(sem->task, sem->sem) == KERN_SUCCESS);
    free(sem);
    puts("PASS: native Mach timed wait, timeout, permits, delayed post and null argument");
}
'''

with tempfile.TemporaryDirectory(prefix="obs-semaphore-test-") as directory:
    harness, binary = Path(directory) / "semaphore.c", Path(directory) / "semaphore"
    harness.write_text(fixture)
    subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], timeout=5, check=True)
