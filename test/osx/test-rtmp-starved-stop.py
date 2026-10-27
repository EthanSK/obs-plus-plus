#!/usr/bin/env python3
"""Run the actual RTMP send/Stop paths when encoded packets stop arriving."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / "plugins/obs-outputs/rtmp-stream.c").read_text()


def function(name):
    start = re.search(r"^static [^\n]*\b" + name + r"\(", source, re.MULTILINE).start()
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


fixture = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define OBS_OUTPUT_SUCCESS 0
#define OBS_OUTPUT_DISCONNECTED 1
#define OBS_OUTPUT_ENCODE_ERROR 2
#define OBS_ENCODER_VIDEO 1
#define OBS_ENCODER_AUDIO 2
#define CODEC_H264 1
struct semaphore { pthread_mutex_t mutex; pthread_cond_t cond; unsigned permits; };
struct encoder_packet { int type, track_idx; size_t size; int64_t sys_dts_usec; };
struct dbr_frame { uint64_t send_beg, send_end; size_t size; };
struct rtmp_stream {
    atomic_bool stopped, active, connecting, disconnected, encode_error;
    void *output, *stop_event;
    uint64_t stop_ts, shutdown_timeout_ts;
    int max_shutdown_time_sec, video_codec[1];
    struct semaphore *send_sem;
    bool sent_headers, dbr_enabled, new_socket_loop, socket_thread_active;
    pthread_t send_thread, socket_thread;
    void *send_thread_signaled_exit, *buffer_has_data_event;
    struct { bool m_bCustomSend; } rtmp;
    struct { const char *array; } path;
    int dbr_cur_bitrate, dbr_orig_bitrate;
    pthread_mutex_t dbr_mutex;
};
static atomic_int waits, sends, released, available_packets, ended;
static uint64_t packet_timestamp;
static uint64_t os_gettime_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static bool stopping(struct rtmp_stream *s) { return atomic_load(&s->stopped); }
static bool active(struct rtmp_stream *s) { return atomic_load(&s->active); }
static bool connecting(struct rtmp_stream *s) { return atomic_load(&s->connecting); }
static bool disconnected(struct rtmp_stream *s) { return atomic_load(&s->disconnected); }
static void join_connect_thread(struct rtmp_stream *s) { (void)s; }
static void os_set_thread_name(const char *s) { (void)s; }
static void os_atomic_set_bool(atomic_bool *p, bool v) { atomic_store(p, v); }
static bool os_atomic_load_bool(atomic_bool *p) { return atomic_load(p); }
static int os_event_signal(void *p) { atomic_store((atomic_bool *)p, true); return 0; }
static void os_event_reset(void *p) { atomic_store((atomic_bool *)p, false); }
static int os_sem_post(struct semaphore *s) {
    pthread_mutex_lock(&s->mutex); ++s->permits; pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex); return 0;
}
static int wait_for_permit(struct semaphore *s, bool timed, unsigned long ms) {
    atomic_fetch_add(&waits, 1);
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ms / 1000; deadline.tv_nsec += (ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&s->mutex);
    int result = 0;
    while (!s->permits && !result)
        result = timed ? pthread_cond_timedwait(&s->cond, &s->mutex, &deadline)
                       : pthread_cond_wait(&s->cond, &s->mutex);
    if (!result) --s->permits;
    pthread_mutex_unlock(&s->mutex); return result;
}
static int os_sem_wait(struct semaphore *s) { return wait_for_permit(s, false, 0); }
static int os_sem_timedwait(struct semaphore *s, unsigned long ms) { return wait_for_permit(s, true, ms); }
static bool get_next_packet(struct rtmp_stream *s, struct encoder_packet *p) {
    (void)s;
    int previous = atomic_load(&available_packets);
    if (!previous) return false;
    atomic_fetch_sub(&available_packets, 1);
    *p = (struct encoder_packet){.type = OBS_ENCODER_VIDEO, .sys_dts_usec = (int64_t)packet_timestamp};
    return true;
}
static bool send_headers(struct rtmp_stream *s) { s->sent_headers = true; return true; }
static bool send_footers(struct rtmp_stream *s) { (void)s; return true; }
static int send_packet(struct rtmp_stream *s, struct encoder_packet *p, bool header) {
    (void)s; (void)p; (void)header; atomic_fetch_add(&sends, 1); return 1;
}
static int send_packet_ex(struct rtmp_stream *s, struct encoder_packet *p, bool h, bool f, int i) {
    (void)f; (void)i; return send_packet(s, p, h);
}
static int send_audio_packet_ex(struct rtmp_stream *s, struct encoder_packet *p, bool h, int i) {
    return send_packet_ex(s, p, h, false, i);
}
static void obs_encoder_packet_release(struct encoder_packet *p) { (void)p; atomic_fetch_add(&released, 1); }
static void dbr_add_frame(struct rtmp_stream *s, struct dbr_frame *f) { (void)s; (void)f; }
static void dbr_set_bitrate(struct rtmp_stream *s) { (void)s; }
static void set_output_error(struct rtmp_stream *s) { (void)s; }
static void RTMP_Close(void *s) { (void)s; }
static void free_packets(struct rtmp_stream *s) { (void)s; }
static void obs_output_signal_stop(void *s, int code) { (void)s; (void)code; atomic_store(&ended, 1); }
static void obs_output_end_data_capture(void *s) { (void)s; atomic_store(&ended, 1); }
#define info(...) ((void)0)
'''
fixture += function("can_shutdown_stream") + function("rtmp_stream_stop") + function("send_thread")
fixture += r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    struct semaphore sem = {.mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};
    struct rtmp_stream s = {.active = true, .send_sem = &sem, .sent_headers = true,
                           .max_shutdown_time_sec = 1};
    s.stop_event = &s.stopped;
    uint64_t started = os_gettime_ns();
    assert(!pthread_create(&s.send_thread, NULL, send_thread, &s));
    while (!atomic_load(&waits)) usleep(1000);
    if (!strcmp(argv[1], "force-empty")) {
        rtmp_stream_stop(&s, 0);
    } else if (!strcmp(argv[1], "graceful-drain")) {
        packet_timestamp = os_gettime_ns() / 1000;
        atomic_store(&available_packets, 1); os_sem_post(&sem);
        while (!atomic_load(&sends)) usleep(1000);
        rtmp_stream_stop(&s, os_gettime_ns());
        usleep(20000);
        assert(!atomic_load(&ended));
        packet_timestamp = os_gettime_ns() / 1000;
        atomic_store(&available_packets, 1); os_sem_post(&sem);
    } else {
        assert(!strcmp(argv[1], "starved") || !strcmp(argv[1], "starved-after-drain"));
        if (!strcmp(argv[1], "starved-after-drain")) {
            packet_timestamp = os_gettime_ns() / 1000;
            atomic_store(&available_packets, 1); os_sem_post(&sem);
            while (!atomic_load(&sends)) usleep(1000);
        }
        rtmp_stream_stop(&s, os_gettime_ns());
    }
    assert(!pthread_join(s.send_thread, NULL));
    assert(atomic_load(&ended) && !active(&s));
    assert(os_gettime_ns() - started < 2000000000ULL);
    if (!strcmp(argv[1], "graceful-drain")) assert(atomic_load(&sends) == 1 && atomic_load(&released) == 1);
    printf("PASS: RTMP Stop %s (%.3f s)\n", argv[1], (os_gettime_ns() - started) / 1e9);
}
'''

with tempfile.TemporaryDirectory(prefix="obs-rtmp-starved-test-") as directory:
    harness, binary = Path(directory) / "starved.c", Path(directory) / "starved"
    harness.write_text(fixture)
    subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(harness), "-o", str(binary)], check=True)
    for case in ("force-empty", "graceful-drain", "starved", "starved-after-drain"):
        subprocess.run([str(binary), case], timeout=3, check=True)
