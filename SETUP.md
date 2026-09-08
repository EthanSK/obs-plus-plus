# My OBS++ setup

[Explore the room](https://ethansk.github.io/ethan-setup/) · [OBS++ changes](FORK-CHANGES.md) · [Aitum++](https://github.com/EthanSK/obs-aitum-stream-suite)

These are the settings I checked on 8 September 2026. My desk and laptop use
different profiles; the currently running laptop profile is not the resolution
of the Dell monitor. Use dimensions that match your own capture.

## Apps and hardware

- **OBS++ 32.2.2** for capture, composition, recording and the built-in stream.
- **Aitum++ 1.2.1 fork** for extra canvases, Twitch/Kick outputs, output health and recovery controls.
- **OBScene** for my display/profile workflow and Aitum's Restart OBS shortcut: [source and releases](https://github.com/EthanSK/OBScene).
- At the desk: **Shure SM7B → Focusrite Scarlett 18i8**, a **Dell S3422DW** main display and a **Samsung S80UA** for OBS. The laptop profile currently uses the Mac microphone and a webcam.
- OBS uses **OpenGL**, **48 kHz stereo**, **NV12**, **Rec. 709**, and **Limited/Partial** range in these profiles.

## Profiles

| Setting | Desk / music | Laptop / coding |
| --- | --- | --- |
| Main canvas | 3440 × 1440 | 4112 × 2658 |
| Main output | 3440 × 1440 | 3288 × 2126 |
| Frame rate | 30 FPS | 30 FPS |
| Extra Aitum canvas | 1080 × 1920, named Vertical | 1670 × 1080, named Platform 1080p |
| Recording container | MKV | MKV |
| Recording encoder | Reuse stream encoder | Reuse stream encoder |
| Recording audio tracks | 1–6 | 1–3 |

MKV is the configured container; remux a completed recording from **File → Remux
Recordings** when an editor needs MP4. The independent audio tracks let me adjust
sources later. Route your own microphone, desktop audio and music deliberately
in Advanced Audio Properties; track numbers alone do not assign sources.

## Current laptop outputs

| Output | Encoder | Video bitrate | Audio |
| --- | --- | --- | --- |
| OBS built-in → Restream | Apple VideoToolbox H.264, CBR, High profile | 15,000 kbps | Main track |
| Aitum → Twitch | Apple VideoToolbox H.264, CBR, High profile, 2-second keyframe interval | 5,000 kbps | CoreAudio AAC, 144 kbps |
| Aitum → Kick | Apple VideoToolbox H.264, CBR, High profile, 2-second keyframe interval | 8,000 kbps | CoreAudio AAC, 144 kbps |

Twitch and Kick use the extra canvas. Their saved output scaling switch is off;
the old 2228 × 1440 dimensions still present in those disabled controls are not
their active output size. Aitum++ aligns the 1670 × 1080 Apple H.264 canvas to
1672 × 1080 to avoid the row-stride corruption described in its change notes.
These are my configured values, not a claim about each platform's current
recommended bitrate or the quality available on every connection.

The main recording reuses the main stream encoder. Aitum's per-output bitrates
and encoder settings remain independent. The OBS++ status bar shows built-in
and Aitum stream health together; its CPU number covers the whole OBS process.

## Recreate the workflow

1. Build OBS++ below, then build and install [Aitum++](https://github.com/EthanSK/obs-aitum-stream-suite#build-on-macos).
2. Grant OBS++ the macOS screen-recording, microphone and camera permissions you need. Keep the device and display selections specific to your machine.
3. Create a separate profile and scene collection. Set the canvas, output size, frame rate, audio format and MKV recording in OBS Settings.
4. Add macOS Screen Capture, your camera and microphone. Add your own browser chat dock if wanted; the website only uses public examples.
5. In **Tools → Aitum++ Settings**, add an extra canvas and its outputs. Put your own service and stream key into each output. Use **Aitum++ Controls** for the extra canvases/Outputs dock, or **Aitum++ Workspaces (Advanced)** for saved dock layouts.
6. Assign recording tracks in Advanced Audio Properties, make a short local recording, then check the resulting file's picture, sound, sync and track separation before going live.
7. Start the intended outputs explicitly and inspect the Outputs dock and OBS++ status bar. Selecting a workspace does not start an output.

No private scene export, browser-dock URL, stream key or service login is
included. Configure those locally; importing someone else's raw OBS profile is
not required to reproduce this setup.

## Build OBS++ on Apple silicon

Use macOS 26+, full Xcode 26+ with its command-line tools selected, Git and
CMake 3.28+. The checked-in macOS preset downloads the pinned OBS dependencies,
Qt and CEF. Allow time and disk space for those downloads and the native build.

```sh
git clone --recurse-submodules https://github.com/EthanSK/obs-plus-plus.git
cd obs-plus-plus
cmake --preset macos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DOBS_VERSION_OVERRIDE=32.2.2-obs-plus-plus \
  -DOBS_CODESIGN_IDENTITY=- \
  -DENABLE_VIRTUALCAM=OFF
cmake --build build_macos --config RelWithDebInfo --parallel 4
```

The app is `build_macos/frontend/RelWithDebInfo/OBS++.app`. Quit OBS before
replacing an installed app, keep a copy of your profile/scene configuration,
then install the built bundle in Applications. OBS++ shares the usual
`obs-studio` settings directory with OBS; it is not an isolated second profile
store. Avoid running both apps against those settings at once.

This example uses local ad-hoc signing and disables the virtual-camera system
extension. It does not produce a notarized public installer. Use your own Apple
signing setup and the upstream build documentation if you need the extension
or intend to distribute a signed app.

The commands and output paths match the repository presets and existing local
build artifacts. This documentation update did not perform a fresh native
build, replace the installed app or test recovery against the live recording.
