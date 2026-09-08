OBS++
=====

My macOS fork of `OBS Studio <https://obsproject.com>`_, with display and camera
recovery fixes and status reporting for multiple Aitum streams.

`My setup <https://ethansk.github.io/ethan-setup/>`_ ·
`Setup and build guide <SETUP.md>`_ ·
`Changes from upstream <FORK-CHANGES.md>`_ ·
`Aitum++ <https://github.com/EthanSK/obs-aitum-stream-suite>`_

What changed
------------

- A disconnected capture target leaves its restart action available; retrying
  refreshes the target list and resolves a reconnected display's saved identity.
- Camera reconnect failures log a missing error safely instead of crashing in
  the error-reporting path.
- The bottom status bar includes built-in and Aitum stream bitrates, dropped
  frames and congestion. Hover for individual outputs; CPU is the shared OBS
  process total, not a separate CPU measurement for each stream.
- macOS app and Chromium helper names use OBS++ consistently.

These are focused changes on top of OBS 32.2.2 source, not a promise that every
capture or USB failure is recoverable. The exact commits and limits are in
`FORK-CHANGES.md <FORK-CHANGES.md>`_.

Use my setup
------------

I use OBS++ with `Aitum++ <https://github.com/EthanSK/obs-aitum-stream-suite>`_ for
extra canvases, independently configured streaming outputs and recovery controls.
The `setup guide <SETUP.md>`_ covers my desk and laptop profiles, audio, encoder
settings, installation and an initial recording check.

**Source distribution:** this fork has no published installer or binary release.
The installed app on my Mac is a local build. The upstream OBS download does not
include these patches. Build the fork if you need them; signing and notarization
for redistributing your own build are separate steps.

Upstream and support
--------------------

OBS Studio captures, composites, records and streams video. OBS++ preserves its
history and is licensed under GPL v2 or later; see `COPYING <COPYING>`_. The code,
artwork and trademarks retain their respective owners' rights. This is an
independent personal fork, not an official OBS release.

- Fork issues: https://github.com/EthanSK/obs-plus-plus/issues
- Upstream documentation: https://obsproject.com/kb
- Upstream build instructions: https://github.com/obsproject/obs-studio/wiki/Install-Instructions
- Upstream contribution rules: https://github.com/obsproject/obs-studio/blob/master/CONTRIBUTING.md
- Support upstream OBS: https://obsproject.com/contribute

Report fork-specific problems here, with a sanitized log and reproduction steps.
Do not include stream keys or private scene exports. Follow upstream's own
contribution policy before submitting anything there.
