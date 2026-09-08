# Changes from OBS Studio

The fork is based on upstream commit `1bf1379fa` (OBS 32.2.2). These commits remain
in the history so each change can be inspected independently.

| Change | What it fixes | Commit |
| --- | --- | --- |
| Missing capture targets | Marks missing displays/windows as failed, refreshes the source properties and target list, and keeps restart available when another attempt is needed. | [e5e78f1e8](https://github.com/EthanSK/obs-plus-plus/commit/e5e78f1e8) |
| Reconnected displays | Resolves the saved display UUID again before retrying because macOS can assign a different display ID. | [b9ca46b5b](https://github.com/EthanSK/obs-plus-plus/commit/b9ca46b5b) |
| Camera reconnect error reporting | Initializes the error value and logs through a literal format string, including when the camera fails without returning an NSError. | [8af6c7785](https://github.com/EthanSK/obs-plus-plus/commit/8af6c7785) |
| Multiple stream status | Includes active/reconnecting Aitum network outputs alongside the built-in stream, with per-output tooltip values and the highest congestion for the status indicator. Aitum recordings are excluded. | [3022d4b9d](https://github.com/EthanSK/obs-plus-plus/commit/3022d4b9d) |
| OBS++ macOS naming | Names the app, executable and CEF helpers consistently so Chromium can find its helper processes. | [efb932ee7](https://github.com/EthanSK/obs-plus-plus/commit/efb932ee7), [ed271cd4d](https://github.com/EthanSK/obs-plus-plus/commit/ed271cd4d) |

## Boundaries

Capture recovery uses OBS's native restart action after a source is marked failed;
it does not reset every source or guarantee a frozen device will recover. Aitum++
provides a convenient button for that action. Camera-service restart and the
OBScene restart shortcut belong to Aitum++, not this OBS core fork.

The multistream status is display-only: it does not start outputs, change
bitrate settings or route audio. CPU remains a whole-process measurement.

This documentation was checked against the source and installed setup on
8 September 2026. It is not a new crash reproduction, native rebuild or fresh
hardware recovery test; those actions would interrupt the active recording.
