# Verified project lessons

## Verify the installed executable separately from capture plug-ins

The combined stream-status change (`3022d4b9d`) lives in the OBS++ executable,
not Aitum++ or the macOS capture plug-ins. Updating those plug-ins does not
install the bottom-bar change. An app version, a source commit, or a successful
restart does not prove that the running executable contains the patch.

For the 10 September 2026 Twitch-only report, the built executable contained
`GetActiveStreamOutputs`, but the installed executable did not; the two SHA-256
hashes also differed. Read-only OBS WebSocket and Accessibility checks confirmed
an active Aitum Twitch output and built-in recording, while the bottom bar
showed no bitrate or dropped frames. The earlier source fix was already built
and pushed; installation was still outstanding.

Before replacing the executable, compare the exact candidate and installed
files, preserve a recoverable rollback, and recheck both built-in and Aitum
outputs. Built-in stream status alone cannot establish that OBS is idle.
Do not replace the whole older build bundle over newer installed capture or
third-party plug-ins. Verify their versions independently and preserve them.

After an authorized restart, test Twitch-only, built-in plus Aitum, and no
streaming output: active bitrates and dropped-frame values use ` + ` separators,
tooltips identify each output, and recording is not counted as a stream.
These installed-runtime acceptance checks remain distinct from source review,
symbol/hash comparison, and signature verification.

## Reveal saved recordings without parsing the displayed message

Pass the actual saved path from both recording-stop and file-split callbacks to
`OBSBasicStatusBar::showRecordingSaved`. The label can be clipped and translated,
so its visible text is not a reliable path source. Encode the file URL and escape
both link text and tooltip HTML; filenames can contain quotes, ampersands, angle
brackets, percent signs and non-ASCII characters. Use Finder's native file
selection API rather than opening the recording in its default player.

Ordinary messages and timeout clearing must remove the previous link interaction
and tooltip. The release build and isolated Qt/Finder check on 14 September 2026
verified exact-file selection and the existing ten-second timeout without
interrupting OBS. This is not installed-runtime acceptance; installation was
deferred while the existing OBS recording was active.

## Balance macOS capture and encoder ownership on failure and teardown

VideoToolbox retains submitted input buffers for as long as encoding needs them.
Release OBS's input ownership after submission, including rejected submissions;
do not depend on a successful asynchronous callback to release it. Save a newly
created compression session before configuring it so normal teardown can clean
up after a configuration failure. Return a creation error before using the
session, and return false (not a nonzero OSStatus) from a failed boolean helper.

CMSimpleQueue stores pointers without managing their lifetimes. Release a
retained sample if enqueue fails, and drain/release remaining samples and the
queue after compression-session invalidation has ended callbacks.

Both screen-capture paths separately retain the incoming and previously displayed
IOSurface. When those references point to the same surface, skipping the texture
rebind must still release the replaced ownership and decrement its use count.
Pointer equality does not make independently acquired references interchangeable.

Run the isolated regression suite against an existing RelWithDebInfo build:

```sh
test_output_dir=$(mktemp -d)
bash test/osx/run-resource-lifetime-tests.sh build_macos "$test_output_dir"
```

The suite calls the actual encoder/capture implementation with controlled failure
paths and real CoreMedia/IOSurface objects. It also runs six tiny software H.264
sessions, including teardown with pending frames. It does not launch OBS, capture
the display, use a hardware encoder, or touch live outputs. Passing it is not
installed-runtime acceptance or proof of the cause of a system-wide leak.

## Distinguish WindowServer loss from the earlier Metal preview leak

The September 2026 crash sequence showed WindowServer exhausting the system
IOSurface limit, followed by an OBS OpenGL/AppKit surface-binding exception.
Compare incident timestamps inside reports rather than their filenames, which
can reflect later report generation. Verify the renderer in the actual crashed
process: the earlier Metal drawable leak cannot explain an OpenGL-only stack.
An app reporting allocation failures can be a victim; establish the owner of
the growing surface group before attributing the system-wide leak to that app.

## Keep failure diagnostics bounded and distinguish their scope

`[OBS++ health]` snapshots use the existing CPU timer, which runs independently
of the built-in stream and also covers Aitum-only streaming. While streaming or
recording, snapshots are limited to every 30 seconds: process CPU/resident memory,
rendering and main-canvas encoding counters, plus each stream's cumulative bytes,
network drops, frame count, congestion and reconnect flag. Compare successive
timestamped snapshots for rates, respecting counter resets at reconnects. Aitum
recordings are not counted as network streams. These metrics do not identify the
owner of system-wide IOSurface allocations.

Reuse the CPU timer's existing sample: `GetCPUUsage()` changes its measurement
baseline, so a second immediate query produces a misleading diagnostic value.
Never log stream settings, URLs, keys, tokens or frame contents for this feature.
Existing OBS log retention remains authoritative; no extra telemetry files or
background process are needed.

VideoToolbox callback failures and ScreenCaptureKit texture failures log the
first event and at most one further report per 30 seconds per instance, retaining
cumulative counters for teardown. Distinguish encoder-dropped frames, callback
errors and full sample queues from RTMP network drops. Capture texture failures
include source/display/surface identity and dimensions, not image contents.

Run `python3 test/osx/test-output-health-logging.py` for deterministic coverage of
the actual health logger/enumerator with read-only OBS stubs; the native ownership
suite additionally tests callback/texture failure throttling. Neither substitutes
for verification of the installed build after a safe restart.

## Keep RTMP connection-worker ownership separate from running state

Stopping during reconnection can call `pthread_join(connect_thread)` while that
worker is still connecting. The old worker detached itself before returning
because Stop had not yet set the stop event. On macOS, that detach can strand the
waiting join indefinitely: a live hang sample showed the UI inside this join,
while network sending and recording continued. A native join/self-detach probe
reproduced the hang without running OBS or changing network settings.

Keep connection workers joinable. Reap the previous attempt before replacing its
handle on reconnect, and reap it on Stop and destruction, including early
connection errors. The joinable flag records owned thread resources, not whether
the worker is still running. Always clear the separate connecting flag on every
exit and after thread-creation failure. Event logs before and after the Stop wait
identify this boundary without adding per-frame logs or a watchdog.

Run `python3 test/osx/test-rtmp-connect-stop.py` for native thread coverage of the
Stop/finish overlap, repeated attempts, early errors and creation failure. This
fix does not remove the existing wait for an in-flight network connection or
prove that Wi-Fi is stable. Correlate RTMP timestamps with macOS Wi-Fi roam events
and bounded gateway latency checks before attributing disconnects to an encoder
or memory leak; a reconnect alone is not evidence of either.

## Show the local release date, not upstream or launch time

The title suffix uses the local source commit's calendar date, embedded by CMake
in `ui-config.h`; it does not query Git, GitHub or the clock while OBS is running.
The Git HEAD reflog is a configure dependency so a later local commit updates the
metadata during an incremental build, including plug-in-only changes. Build and
install a committed release; a source archive without Git must explicitly supply
`OBS_PLUS_PLUS_RELEASE_DATE` in ISO date form. Do not replace it with the app's
launch time, installation file timestamp or an upstream OBS tag date.

`test/osx/test-release-title.py` checks the actual title method with Qt, preserving
Studio/safe/portable/profile/scene text and appending the date once, plus CMake
metadata refresh after a new source commit. The installed title still requires
an authorized restart and native UI readback; do not interrupt live outputs just
to check the label.

When reconfiguration reports imported library include paths inside a removed
Xcode SDK, refresh only the affected CMake cache entries (CURL, Iconv, OpenGL and
ZLIB here) against the SDK reported by `xcrun --sdk macosx --show-sdk-path`.
Do not change source include paths or recreate the removed SDK directory.
