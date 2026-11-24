# To do

Work done on 2026-11-24 sits on the local branch `merge-upstream-prs`: upstream PRs #18, #37 and #38 as merge commits, and everything else uncommitted on top. It builds on the host with no warnings and the simulator starts. On 2026-11-24 build `241120261728` was flashed on the R1 (which had Compas firmware on it) and boots to the player; nothing else below has been tried on it, and nothing at all on the R3 Pro II. The image carries upstream's kernel and modules, not the nine changed module sources. The Compas image is on the R1's card as `compas-r1-backup.bin` (rename to `r1.upt` to go back).

## Before the first flash

- [x] `make target -j$(nproc)` in `sonix-player/`: passed `check-abi: no symbol beyond glibc 2.22`. The toolchain script also needs `gtk-doc-tools`, `gettext`, `pkg-config` and `libglib2.0-dev-bin` on the host; resume a failed run with `RBDEV_RESTART=<step> make target`.
- [ ] Build `hiby-custom-kernel/` (now a folder of this repository) with the SDK: nine module sources were changed and have never been compiled (`mp2731`, `soc_pwm`, `sa_hgl_dma`, `gt9xx_touch`, `cw2015`, `soc_aic`, `codec_cs43198_dual`, both sound cards).
- [x] Run `sonix-packer/sonix_firmware_packer.sh` (R1 only so far; rootfs is 47063040 of 47185920 bytes, so about 120 KB is left). It now stops if the overlay's modules do not match the kernel going into the image, and if the stock `.upt` is the other model's.
- [x] Flash ONE device you can recover (stock firmware `.upt` ready on a card), charged above 30%: the R1, over ADB (`bootmode.sh Recovery`, then `reboot`).
- [ ] Turn on Settings > Developer options > Log to microSD before testing, and keep the log.

## Device test: the basics first

- [ ] Boots to the player, on the R3 Pro II and on the R1. No boot loop. (R1: yes, the process stays up, card mounted, ADB on. R3 Pro II: not tried.)
- [ ] Boot with no card in, then insert one: library, log and thumbnails attach.
- [ ] Full library scan of a real card finishes. Note how long it takes compared with before (the scan now reads each track's length).
- [ ] Star tracks, reorder a playlist and start a folder WHILE a scan runs: the scan must not stop with "database error".
- [ ] Play: FLAC, MP3, M4A, WAV (16 and 24 bit, 44.1 to 192 kHz), DSF and DFF to the end of the track, Opus, WavPack, a float AIFF or CAF.
- [ ] Gapless and fade between tracks, seek, pause and resume (listen for a dropped or repeated fraction of a second).
- [ ] ReplayGain on a loud track with a gain above +6 dB: no clicks.
- [ ] Unplug a USB DAC in the middle of a hi-res WAV: no crash.
- [ ] Standby and wake: screen off, wait past the standby time, wake with the power key. Auto power-off still fires.
- [ ] Standby during a library scan: the device must stay awake until the scan ends.

## Device test: the risky changes

- [ ] **USB storage mode.** Plug into a PC in each of these states and check the card appears: idle; music playing; an e-book open; a game open; a podcast downloading. It now refuses instead of forcing the unmount when something holds a file. If it refuses in ordinary use, that needs fixing before release. The popup on refusal says "Storage available again", which is the wrong message.
- [ ] Copy a large folder in the file manager, then plug into a PC: the copy stops, the card is not corrupted.
- [ ] Config survives pulling power right after changing a setting.
- [ ] Firmware update is refused below 30% battery without a charger, from both SD and internet.
- [ ] Charge limit holds on the charging screen (device off, on the charger) on the R3 Pro II.
- [ ] Bluetooth headphones: connect, volume keys follow, switch Bluetooth off and on, volume still follows.
- [ ] Bluetooth receiver mode with a phone sending aptX-HD if you have one (24-bit path changed).
- [ ] Wi-Fi with a password containing `"` or `\` (suspected bug, NOT fixed, never confirmed).

## Device test: new features

- [ ] **Last.fm QR sign-in** (needs your API key in `streaming-keys.ini`, sealed): the code appears, approving on the phone signs the player in, a scrobble arrives. Then the password route.
- [ ] **Your own API keys**: with no keys in the firmware, a `streaming-keys.ini` in the card's root folder with Last.fm and Podcast Index keys turns those two on after a restart (the log says `streamkeys: from ...`); Tidal and Qobuz still say they have no keys; without the file all four pages say where to put it. Not read when the card goes in after start-up.
- [x] **No false "Not removed safely" after an update** (R1, 2026-11-25, update from the card): a clean card came back from the recovery system marked, which settles that it is recovery that marks it; with the fix the log said `was marked by the firmware update just done; the mark is lowered`, the kernel mounted the card without complaint, the page said Ready, and it still said Ready after an ordinary restart. Not tried: the same over the internet (it shares the hand-over), an exFAT card, and a card that was already marked before the update.
- [ ] **SD card page** (Settings > System > SD card): the figures and the file system are right for a FAT32 and an exFAT card; State says "Not removed safely" after the card is pulled while playing; the speed test gives plausible figures and leaves no file behind. **Format, on a card with nothing to lose**: it asks twice, formats, the card mounts as FAT32 and a rescan works; it refuses with a book or a video open; plugging into a PC during a format does not export the card; Windows and macOS read the formatted card (the partition type byte is left as it was).
- [ ] **Level meter** (Settings > Music > Now Playing; now the Compas meter ported: bars, dials, spectrum, timed to the sound): each shape moves with the music in Standard and Studio layouts, a tap on it moves to the next shape and a swipe there still opens the lyrics, rests on pause and on DSD, stops with the screen off. Check CPU for each shape (the spectrum is the costly one) at 44.1 and 192 kHz, and whether it is in time with the sound on the socket and over Bluetooth.
- [ ] **Video** (the FFmpeg player from `Hiby-R1-Mod/sonix-player-video-player-patches`, which replaced the MPEG-1 one): More > Video lists the card's `Video` folder; files also open from the File manager. Convert with `tools/video-convert.sh` (see `VIDEO.md`). Check frames shown per 10 s in the log, lip sync on the socket and on Bluetooth, seek, resume, the keys, leaving the page, and that music plays normally afterwards.
- [ ] **Track lengths**: on rows and in search after a rescan; the album total in the alternative album view; "Detect changes" filling in an old library over a few runs.
- [ ] **Long press on a cut-off title** scrolls it; selection mode still starts.
- [ ] **Auto sleep timer**: set a window around the current time, playback pauses after the timer's length, a button press restarts the countdown.
- [ ] **Jump back / forward** in button remap on both models; hold and click-then-hold on a wired remote.
- [ ] **Podcasts**: Details shows the full title; Mark all as heard on a show with many episodes.
- [ ] **Auto power-off** at 12, 24 and 48 hours (at least that the options save and show).
- [ ] **ReplayGain on an `.m4a` tagged by MusicBrainz Picard** (upstream issue #9).
- [ ] **Game Boy**: a save survives power-off from the power menu; touch-only play keeps the screen on; an old `.sav` is still found. Listen for a hitch every 30 seconds in a game that saves constantly.
- [ ] **From the merged PRs**: an SACD ISO scans and plays (#38); the custom accent colour picker (#37); Hebrew titles display (#18).

## Decisions still open

- [ ] SonixLink has no password, listens on every interface and starts at boot.
- [ ] The Bluetooth agent accepts any pairing request without asking.
- [ ] The Wi-Fi transfer server runs as root with no authentication while its page is open.
- [x] OTA: `ota-repo` in both `system-info.json` files now names this fork (`bidhata/bidhata-sonix-h1`), so an internet update no longer installs upstream's image. The fork has no releases yet. Still open: images are checked by size only when the release has no digest.
- [ ] Last.fm: the account owning the API key needs Pro from 2026-12-01.
- [ ] PR #37 brings a `docs/` folder, which the PR template forbids. Licence of the Hebrew fonts from PR #18 is unchecked.
- [ ] `readme-old.md` and the packer's header still say `kernel/xImage-<MODEL>` is optional; with the overlay's modules it no longer is.
- [ ] How to split the uncommitted work into commits (audit fixes, Last.fm, each feature).

## Known gaps, not fixed

- Stopping a radio station whose host does not answer can freeze the UI for about ten seconds (needs an abortable connect in `net/http.c`).
- HE-AAC `.aac` files may play at half speed (needs fdk-aac source or a device to confirm).
- QuickTime v1/v2 sound descriptions in MP4 are refused.
- USB gadgets are not unbound before suspend when ADB is on with no cable.
- Four condition variables still time out on the wall clock, so setting the clock back stretches some waits.
- E-book scroll mode builds every picture of a chapter at once.
- A game is not paused or warned when the card is exported or removed.
- VBR MP3s without a Xing/VBRI header and raw `.aac` files get no length.
- Video files show the generic file icon in the File manager.

## Upstream issues not started

- #10 albums by year in Artists, continue to the artist's next album.
- #74 podcast auto-download and delete, #42 podcast and radio sorting and tags, #7 podcast playlists (and the playlist part of #44).
- #16 real crossfade.
- #12 Bluetooth gamepad for Gearboy, #75 wake from the inline remote: feasibility unknown without hardware.
- #17 SoundCloud: needs API credentials.
- #40 run the binary from the SD card: a contributor is writing it upstream.
- #43: assignable remote hold, hold on USB-C remotes, more multi-tap codes.
