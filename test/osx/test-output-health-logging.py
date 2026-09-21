#!/usr/bin/env python3
"""Compile the actual health logger/enumerator against deterministic read-only OBS stubs."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / "frontend/widgets/OBSBasicStatusBar.cpp").read_text()
enumerator = source.split("struct StreamOutputStatus {", 1)[1].split("OBSBasicStatusBar::OBSBasicStatusBar", 1)[0]
logger = source.split("void OBSBasicStatusBar::LogOutputHealth(double cpuUsage)", 1)[1].split("void OBSBasicStatusBar::UpdateCurrentFPS()", 1)[0]

fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
struct QString {
    std::string text;
    static QString fromUtf8(const char *value) { return {value}; }
};
#define QStringLiteral(value) QString::fromUtf8(value)
constexpr char aitumOutputPrefix[] = "Aitum Stream Suite Output ";
struct obs_output_t {
    const char *name;
    const char *id;
    bool active, reconnecting, service;
    uint64_t bytes;
    int dropped, frames;
    float congestion;
};
using OBSOutput = obs_output_t *;
OBSOutput OBSGetStrongRef(OBSOutput output) { return output; }
const char *obs_output_get_name(OBSOutput output) { return output->name; }
const char *obs_output_get_id(OBSOutput output) { return output->id; }
bool obs_output_active(OBSOutput output) { return output->active; }
bool obs_output_reconnecting(OBSOutput output) { return output->reconnecting; }
bool obs_output_get_service(OBSOutput output) { return output->service; }
uint64_t obs_output_get_total_bytes(OBSOutput output) { return output->bytes; }
int obs_output_get_frames_dropped(OBSOutput output) { return output->dropped; }
int obs_output_get_total_frames(OBSOutput output) { return output->frames; }
float obs_output_get_congestion(OBSOutput output) { return output->congestion; }
std::vector<OBSOutput> outputs;
void obs_enum_outputs(bool (*callback)(void *, OBSOutput), void *data) {
    for (auto output : outputs) if (!callback(data, output)) break;
}
uint64_t now = 1000000000ULL;
uint64_t os_gettime_ns() { return now; }
struct os_proc_memory_usage_t { uint64_t resident_size = 0; };
bool memory_available = true;
bool os_get_proc_memory_usage(os_proc_memory_usage_t *usage) {
    if (memory_available) usage->resident_size = 512ULL * 1024 * 1024;
    return memory_available;
}
struct video_t {};
video_t video;
video_t *obs_get_video() { return &video; }
unsigned video_output_get_skipped_frames(video_t *) { return 2; }
unsigned video_output_get_total_frames(video_t *) { return 300; }
unsigned obs_get_lagged_frames() { return 3; }
unsigned obs_get_total_frames() { return 301; }
double obs_get_active_fps() { return 30; }
uint64_t obs_get_average_frame_time_ns() { return 2000000; }
struct OBSBasic { double GetCPUUsage() { return 1.5; } } main_window;
template<typename T> T qobject_cast(void *object) { return static_cast<T>(object); }
constexpr int LOG_INFO = 200;
std::vector<std::string> logs;
void blog(int, const char *format, ...) {
    char text[2048]; va_list args; va_start(args, format);
    vsnprintf(text, sizeof(text), format, args); va_end(args); logs.emplace_back(text);
}
class OBSBasicStatusBar {
public:
    uint64_t lastHealthLogTime = 0;
    OBSOutput streamOutput = nullptr, recordOutput = nullptr;
    void *parent() { return &main_window; }
    void LogOutputHealth(double cpuUsage = 1.5);
};
'''
fixture += "struct StreamOutputStatus {" + enumerator
fixture += "void OBSBasicStatusBar::LogOutputHealth(double cpuUsage)" + logger
fixture += r'''
int main() {
    obs_output_t built_in{"adv_stream", "rtmp_output", true, false, true, 12345, 12, 100, 0.8f};
    obs_output_t twitch{"Aitum Stream Suite Output Twitch", "rtmp_output", true, false, true, 5555, 2, 50, 0.1f};
    obs_output_t record{"Aitum Stream Suite Output Record", "ffmpeg_muxer", true, false, false, 9999, 0, 50, 0};
    outputs = {&built_in, &twitch, &record};
    OBSBasicStatusBar bar;
    bar.streamOutput = &built_in; bar.recordOutput = &record;
    bar.LogOutputHealth();
    assert(logs.size() == 3);
    assert(logs[0].find("resident_mib=512.0 memory_available=1") != std::string::npos);
    assert(logs[0].find("render_lag=3/301 main_encode_lag=2/300 recording=1 streams=2") != std::string::npos);
    assert(logs[1].find("output='adv_stream' bytes=12345 network_dropped=12") != std::string::npos);
    assert(logs[2].find("output='Aitum Stream Suite Output Twitch'") != std::string::npos);
    puts("PASS: combined built-in/Aitum snapshot reports process health and separates recording");
    for (int i = 1; i < 30; i++) { now += 1000000000ULL; bar.LogOutputHealth(); }
    assert(logs.size() == 3);
    now += 1000000000ULL; bar.LogOutputHealth(); assert(logs.size() == 6);
    puts("PASS: repeated timer calls emit at most one snapshot per 30 seconds");
    logs.clear(); built_in.active = false; twitch.active = false; twitch.reconnecting = true;
    bar.streamOutput = nullptr; now += 30000000000ULL; bar.LogOutputHealth();
    assert(logs.size() == 2 && logs[1].find("reconnecting=1") != std::string::npos);
    puts("PASS: Aitum-only reconnect is logged without a built-in stream");
    logs.clear(); twitch.reconnecting = false; now += 30000000000ULL; bar.LogOutputHealth();
    assert(logs.size() == 1 && logs[0].find("recording=1 streams=0") != std::string::npos);
    puts("PASS: recording-only health is retained");
    logs.clear(); record.active = false; now += 30000000000ULL; bar.LogOutputHealth();
    assert(logs.empty() && bar.lastHealthLogTime == 0);
    puts("PASS: no periodic health logs while all outputs are idle");
    record.active = true; memory_available = false; bar.LogOutputHealth();
    assert(logs.size() == 1 && logs[0].find("memory_available=0") != std::string::npos);
    puts("PASS: memory-query failure is explicitly marked unavailable");
}
'''

with tempfile.TemporaryDirectory(prefix="obs-health-test-") as directory:
    harness = Path(directory) / "health.cpp"
    binary = Path(directory) / "health"
    harness.write_text(fixture)
    subprocess.run(["xcrun", "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

cpu_timer = source.split("void OBSBasicStatusBar::UpdateCPUUsage()", 1)[1].split("void OBSBasicStatusBar::LogOutputHealth(double cpuUsage)", 1)[0]
assert "LogOutputHealth(cpuUsage);" in cpu_timer and cpu_timer.count("GetCPUUsage()") == 1
assert "GetCPUUsage()" not in logger
print("PASS: always-running CPU timer supplies its existing sample without querying again")
