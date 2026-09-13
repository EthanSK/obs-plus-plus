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
