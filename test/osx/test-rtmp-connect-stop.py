#!/usr/bin/env python3
"""Exercise actual RTMP connection-worker ownership with deterministic native threads."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / "plugins/obs-outputs/rtmp-stream.c").read_text()


def function(name):
    start = re.search(r"^static [^\n]*\b" + name + r"\(", source, re.MULTILINE).start()
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
enum { OBS_OUTPUT_SUCCESS, OBS_OUTPUT_BAD_PATH, OBS_OUTPUT_HDR_DISABLED };
enum { CODEC_H264 = 1, CODEC_HEVC, VIDEO_CS_2100_HLG, VIDEO_CS_2100_PQ };
#define MAX_OUTPUT_VIDEO_ENCODERS 1
typedef struct {} obs_encoder_t;
typedef struct {} video_t;
struct video_output_info { int colorspace; } video_info;
struct rtmp_stream {
    _Atomic bool connecting, active, stopped;
    bool connect_thread_joinable;
    pthread_t connect_thread;
    void *output, *stop_event, *send_sem;
    uint64_t stop_ts, shutdown_timeout_ts;
    int max_shutdown_time_sec, video_codec[1];
    struct { const char *array; } path;
};
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool worker_entered, allow_finish, gated;
static bool init_ok = true, encoder_ok = true, create_ok = true;
static int result_code, joins;
static bool connecting(struct rtmp_stream *s) { return atomic_load(&s->connecting); }
static bool active(struct rtmp_stream *s) { return atomic_load(&s->active); }
static bool stopping(struct rtmp_stream *s) { return atomic_load(&s->stopped); }
static void os_atomic_set_bool(_Atomic bool *p, bool v) { atomic_store(p, v); }
static void os_set_thread_name(const char *n) { (void)n; }
static void os_event_signal(void *event) { atomic_store((_Atomic bool *)event, true); }
static void os_sem_post(void *sem) { (void)sem; }
static void obs_output_signal_stop(void *output, int code) { (void)output; result_code = code; }
static bool obs_output_can_begin_data_capture(void *o, int f) { (void)o; (void)f; return true; }
static bool obs_output_initialize_encoders(void *o, int f) { (void)o; (void)f; return encoder_ok; }
static bool init_connect(struct rtmp_stream *s) { (void)s; return init_ok; }
static obs_encoder_t *obs_output_get_video_encoder2(void *o, size_t i) { (void)o; (void)i; return NULL; }
static video_t *obs_encoder_video(obs_encoder_t *e) { (void)e; return NULL; }
static const struct video_output_info *video_output_get_info(video_t *v) { (void)v; return &video_info; }
static int try_connect(struct rtmp_stream *s) {
    pthread_mutex_lock(&gate_mutex);
    worker_entered = true; pthread_cond_broadcast(&gate_cond);
    while(gated && !allow_finish) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
    if(gated) { const struct timespec delay = {.tv_nsec = 100000000}; nanosleep(&delay, NULL); }
    atomic_store(&s->active, true);
    return OBS_OUTPUT_SUCCESS;
}
static int test_join(pthread_t thread, void **result) {
    pthread_mutex_lock(&gate_mutex);
    allow_finish = true; pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
    ++joins;
    return pthread_join(thread, result);
}
static int test_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *data) {
    return create_ok ? pthread_create(thread, attr, fn, data) : 11;
}
#define pthread_join test_join
#define pthread_create test_create
#define info(...) ((void)0)
'''
if "static void join_connect_thread(" in source:
    fixture += function("join_connect_thread")
fixture += function("rtmp_stream_stop") + function("connect_thread") + function("rtmp_stream_start")
fixture += r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    struct rtmp_stream s = {0}; s.stop_event = &s.stopped;
    if(!strcmp(argv[1], "overlap")) {
        gated = true;
        assert(rtmp_stream_start(&s));
        pthread_mutex_lock(&gate_mutex);
        while(!worker_entered) pthread_cond_wait(&gate_cond, &gate_mutex);
        pthread_mutex_unlock(&gate_mutex);
        rtmp_stream_stop(&s, 0);
        assert(!connecting(&s) && !s.connect_thread_joinable && stopping(&s));
        assert(joins == 1);
    } else if(!strcmp(argv[1], "cycles")) {
        for(int i = 0; i < 100; ++i) {
            atomic_store(&s.stopped, false); atomic_store(&s.active, false);
            assert(rtmp_stream_start(&s));
        }
        rtmp_stream_stop(&s, 1000000000ULL);
        assert(joins == 100 && !s.connect_thread_joinable);
        assert(s.stop_ts == 1000000ULL);
    } else if(!strcmp(argv[1], "init-error")) {
        init_ok = false;
        assert(rtmp_stream_start(&s));
        rtmp_stream_stop(&s, 0);
        assert(!connecting(&s) && !s.connect_thread_joinable && joins == 1);
    } else if(!strcmp(argv[1], "hdr-error")) {
        s.video_codec[0] = 3; video_info.colorspace = VIDEO_CS_2100_HLG;
        assert(rtmp_stream_start(&s));
        rtmp_stream_stop(&s, 0);
        assert(!connecting(&s) && !s.connect_thread_joinable && joins == 1);
    } else if(!strcmp(argv[1], "create-error")) {
        create_ok = false;
        assert(!rtmp_stream_start(&s));
        assert(!connecting(&s) && !s.connect_thread_joinable && joins == 0);
    } else if(!strcmp(argv[1], "encoder-error")) {
        encoder_ok = false;
        assert(!rtmp_stream_start(&s));
        assert(!connecting(&s) && !s.connect_thread_joinable && joins == 0);
    } else { abort(); }
    printf("PASS: RTMP connection ownership %s\n", argv[1]);
}
'''

with tempfile.TemporaryDirectory(prefix="obs-rtmp-stop-test-") as directory:
    harness = Path(directory) / "rtmp-stop.c"
    binary = Path(directory) / "rtmp-stop"
    harness.write_text(fixture)
    subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(binary)], check=True)
    for case in ("overlap", "cycles", "init-error", "hdr-error", "create-error", "encoder-error"):
        subprocess.run([str(binary), case], timeout=5, check=True)

assert "pthread_detach" not in function("connect_thread")
destroy = function("rtmp_stream_destroy")
assert destroy.index("join_connect_thread(stream)") < destroy.index("RTMP_TLS_Free")
print("PASS: connection workers stay joinable and are reaped before destruction")
