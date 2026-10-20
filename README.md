# Recorder

A small GNOME tray app for Linux that records meeting audio on demand and saves it
to a folder, ready for a speech-to-text model. Start and stop from the top bar;
it records your microphone and, by default, the system audio (so remote
participants are captured too), writes Ogg/Opus (or WAV), and starts at login.

Transcription itself is out of scope; this only produces the files.

## Install (Ubuntu 24.04)

```
sudo add-apt-repository ppa:tonkec/recorder
sudo apt update
sudo apt install recorder
```

This installs `/usr/bin/recorder`, a launcher in the applications menu and the
tray icons. Launch *Recorder* once from Activities; it then writes its own
per-user autostart entry (see below). `sudo apt remove recorder` removes the
program but, like any package, leaves per-user files alone:
`~/.config/Recorder/` (settings, lock file), `~/.config/autostart/recorder.desktop`
and any `~/.local/share/icons/hicolor/*/apps/recorder*.png` written by an
earlier source install. Recordings in `~/Recordings` are never touched.

If you previously ran `make install`, remove that copy first (`make uninstall`):
the autostart entry keeps pointing at `~/.local/bin/recorder` as long as that
binary exists.

## Building from source (Ubuntu 24.04)

```
make deps
```

installs `build-essential cmake ninja-build pkg-config qt6-base-dev libpulse-dev
libopusenc-dev`. Everything links dynamically against the system packages:
libpulse must match the running sound server (PipeWire's PulseAudio layer here)
and libopusenc is one small package, so nothing is bundled. The tray icon
needs a StatusNotifier host; on GNOME that is the `ubuntu-appindicators`
extension, enabled by default on Ubuntu.

## Build, run, install

| target | what it does |
|---|---|
| `make build` | configure (Ninja) and compile app + tests into `build/` |
| `make test` | run the unit tests (no sound server needed; the one live test skips itself) |
| `make test-asan` / `make test-tsan` | same under AddressSanitizer / ThreadSanitizer, each in its own build dir |
| `make run` | stop any running instance, launch `build/recorder` without touching the autostart entry |
| `make install` | install to `~/.local/bin`, write the autostart entry for that binary, restart the app |
| `make uninstall` | stop it and remove binary, launcher, autostart entry and icons |
| `make stop` | ask the running instance to quit (it finalizes the file first) |
| `make icons` | regenerate `data/icons/` from `TrayIcon::render` (commit the result) |
| `make bump` | next packaging revision (`+ppaN`) in `debian/changelog`; `MSG="..."` sets the entry text |
| `make release VERSION=x.y.z` | new upstream version in `CMakeLists.txt` and `debian/changelog` |
| `make deb` | build an unsigned `.deb` into `dist/` from the last commit and run lintian |
| `make ppa` | build a signed source package and upload it to the PPA (`SERIES=noble` by default) |

`recorder --version` prints the version and works without a display.

### Publishing a new version

Versions never change on their own. Launchpad rejects a version it has already
seen, so every upload needs a bump first. Pick one:

```
# A fix or small change: next packaging revision, CMake version unchanged
make bump MSG="Fix alignment when the monitor stalls"       # 0.1.0+ppa1 -> 0.1.0+ppa2

# A new upstream version: updates CMakeLists.txt and debian/changelog together
make release VERSION=0.1.1 MSG="Add WAV output"             # -> 0.1.1+ppa1
```

Then commit and upload:

```
git commit -am "Release 0.1.0+ppa2"
make ppa
```

Launchpad builds the package within minutes to an hour; users get it on their
next `sudo apt upgrade`. Leave `MSG` out and `dch` opens an editor for the
changelog text.

How it fits together: the committed version is `<upstream>+ppaN` in
`debian/changelog`, and `project(Recorder VERSION x.y.z)` in `CMakeLists.txt`
holds only the numeric part (CMake rejects `+ppaN`). `make export` refuses to
build if the two disagree, and appends `~<series>1` (e.g. `0.1.0+ppa2~noble1`)
so the local `.deb` from `make deb` and the PPA build carry the identical
version. Build and upload only from a clean commit: `make export` uses
`git archive HEAD`, so uncommitted edits are silently left out.

`recorder --sync-autostart` writes or removes `~/.config/autostart/recorder.desktop`
according to the *Launch at login* setting and exits; `make install` uses it so
installation never depends on a running instance. A normal launch only writes
the entry when it is missing or points at a binary that no longer exists, so a
development build never overwrites the installed entry (`--no-autostart-sync`
disables even that). `--record-seconds N` records once from the command line
and exits (handy for testing the audio path).

Only one instance runs at a time (`~/.config/Recorder/recorder.lock`); a second
launch exits silently.

## Using it

The tray menu has **Start recording** / **Stop recording (12:34)**,
**Open recordings folder**, **Settings…** and **Quit**. Both mouse buttons open
the menu on GNOME. A notification confirms the start and reports the outcome at
the end.

Settings (`~/.config/Recorder/settings.json`):

| key | default | meaning |
|---|---|---|
| `outputDir` | `~/Recordings` | where files go |
| `format` | `opus` | `opus` (Ogg/Opus) or `wav` |
| `captureSystemAudio` | `true` | also record the default output's monitor |
| `launchAtLogin` | `true` | keep an autostart entry |
| `opusBitrate` | `32000` | bits per second for Opus |
| `systemGain` | `1.0` | gain applied to the system audio before mixing |
| `micDevice` | `""` | PulseAudio source name; empty = default |

Files are named `Recording_2026-10-20_14-05-09.ogg` (`_sys` before the extension
when system audio was mixed in) and written as `.part` until they are finalized
and renamed. If the app crashes, the next launch repairs stray `.part` files
(WAV headers get their sizes patched) and renames them.

## Why mono 48 kHz Ogg/Opus at 32 kbps

Speech-to-text models work on mono speech; Opus at 32 kbps VBR in VOIP mode is
transparent for that purpose and weighs about 14 MB per hour, so a recording
of up to roughly 1 h 44 min stays under the 25 MB upload cap used by several
transcription APIs. The container is Ogg with the `.ogg` extension, which the
common services and tools (Whisper, gpt-4o-transcribe, Gemini, Deepgram,
whisper.cpp, ffmpeg) all accept. WAV (PCM16, about 345 MB per hour) is
available when you want lossless audio.

## Alignment policy (microphone + system audio)

The two streams come from two PulseAudio record streams with independent
buffering and, with a USB headset, independent clocks. Recorder mixes them
on the microphone's timeline:

- Every block of samples carries its stream position (frames since the stream
  started, server-reported holes included), and the server's timing
  information maps positions to system time (`timestamp` is the moment the
  server measured, `write_index` the newest sample then, `source_usec` how
  long before that it was captured, `sink_usec` for a monitor how long after it
  will be heard). No transport term is used: it only affects when the reply
  reached us, which the mapping does not depend on.
- Output frame *k* is microphone frame `trim + k`; the monitor is placed on
  that timeline by mapped time, never by arrival time, so late delivery never
  shifts audio.
- Each stream is consumed strictly sequentially. Timestamps are used only for
  the initial offset, the drift accumulator and resynchronisation after a stall.
- If one stream delivers nothing while the other has advanced 500 ms, silence is
  substituted for the missing stream (counted as a "gap patched" in the stop
  notification). When it resumes, audio that belongs to an already-emitted
  interval is dropped; audio that is ahead is padded into place. Treating audio
  that arrives more than 500 ms late as lost is an application policy that
  bounds the damage and keeps the output deterministic.
- Clock drift between the two streams is tracked with the timing information.
  Once the monitor is 100 ms (4800 frames) ahead or behind, one 20 ms chunk is
  dropped or padded; at 100 ppm that is the first correction after about
  17 minutes and then one every 3 min 20 s. Adaptive resampling is future work.
- If no audio at all arrives for 5 s (frozen server, unplugged device) the
  session ends and the file so far is saved as "stopped early".
- If timing information is unavailable the streams are mixed in arrival order
  (flagged in the result).

## Layout

```
src/core/   capture (PulseCapture), queues, clocks and alignment, writers,
            the EncoderThread and the Recorder state machine, settings,
            autostart, tray helpers — a static library with no UI
src/ui/     tray icon + menu, settings dialog
src/main.cpp  wiring
tests/      one QtTest executable per module
```
