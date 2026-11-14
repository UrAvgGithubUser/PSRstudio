<div align="center">

# PSRstudio

**A real-time software arranger, using Yamaha SFF2 Style files to provide auto-accompaniment by following the chords you play.**

</div>

---


## IMPORTANT NOTICE

PSRstudio was developed ALMOST ENTIRELY with a combination of Claude, Deepseek and Z.ai. As such, expect the code base to a maze of terribly written, unoptimized and sloppy code. I'm planning to refactor the code properly one day when I actually learn C++. I apologize in advance for the vibe-coded nature of the app.

## What is it?

PSRstudio is a arranger software, similar to vArranger2, Giglad and One Man Band. Load a Yamaha style file (.sty/.prs/.sst/etc.), and PSRstudio provides auto-accompaniment by transposing the notes in real-time to match the chords you're playing, just like a physical arranger keyboard like the Yamaha Genos and PSR series keyboards.

Built with **Qt 6** and **C++20**, with **FluidSynth** for SoundFont playback and WinMM for hardware MIDI devices.

## Features

- **Style playback** — Support for SFF2 and SFF GE Styles, and rudimentary support for Ketron and Korg style.
- **MIDI playback** — Play back MIDI files (.mid) with the selected Synth backend,
- **Live chord tracking** — Fingered and single-finger chord recognition modes along with a configurable split point.
- **Real-time transposition** — Transposes the notes according to CASM and transposition rules.
- **Mixer** — Mix and master each channel.
- **MegaVoice support** — Detects MegaVoice styles, and plays them properly on supported keyboards. Can be patched to fix how they sound on unsupported MIDI hardware or Soundfonts.
- **Remap profiles** — Configurable `.remap` file, allowing you to create a remap table for your own Synth backend, making styles play back accurately. 
- **Two synth backends** — Hardware MIDI out or FluidSynth with any SoundFont.

## Building instructions

### Prerequisites

- **Windows** 10/11
- **Visual Studio 2022** with the C++ desktop workload
- **Qt 6** (Widgets, Gui, Core) — msvc build
- **FluidSynth 2.x** — fetched via [vcpkg](https://vcpkg.io) (`vcpkg.json` manifest included)
- **CMake** 3.21+ (bundled with Visual Studio)

### Steps

```bat
:: configure (vcpkg toolchain resolves FluidSynth automatically)
cmake -B build -S . -G "Visual Studio 17 2022" ^
      -DCMAKE_TOOLCHAIN_FILE=<your-vcpkg-root>/scripts/buildsystems/vcpkg.cmake

:: build
cmake --build build --config Release
```

The repo vendors everything else it needs (the JUCE audio modules live in `lib/JUCE`).

> **Tip** — the easiest way is opening the folder in Visual Studio (*File → Open → CMake*): `CMakeSettings.json` is already configured.

## Usage

1. **Pick a sound destination** — connect a hardware MIDI device (WinMM) or select a SoundFont file for the built-in FluidSynth
2. **Load a style** — *Load Style / MIDI…* and choose any Yamaha style or MIDI file
3. **Play chords** on your MIDI keyboard — Start/Stop, fills, breaks and endings are one click away

### Data files

PSRstudio keeps its user data in `Documents\PSRstudio\`:

| File / folder | Purpose |
|---------------|---------|
| `voices.csv` | Voice library for the mixer's voice picker — one line per voice: `name,msb,lsb,pc[,category]` |
| `remaps\*.remap` | Remap profiles: voice/bank/PC translation tables + MONTAGE settings, selectable in the UI |
| `mixer\*.png` | Optional custom artwork for the mixer knobs and sliders (replaces the built-in art) |
| `PSRstudio.ini` | All settings (backend, ports, mixer state, last style) — saved automatically |

## Project layout

```
mainwindow.*            Qt GUI (main window, mixer, voice selector, splash)
player.*                Style playback engine (scheduler, held notes, retrigger)
playback_controller.*   Arranger: chord tracking → per-note transposition
transpose.*             NTR/NTT transposition engine + MegaVoice handling
chord_engine.*          MIDI chord detection (fingered / single finger)
style.*                 Yamaha style file parser (SFF1/SFF2, Ketron, Korg)
remap.*                 Voice remap profiles + destination-synth settings
channel_model.*         Live 16-channel state mirrored to the synth and mixer
midi*.*                 WinMM MIDI input/output, SMF player
fluidsynth_output.*     FluidSynth backend
audio_engine.*          JUCE audio backend (EQ + limiter master chain)
```

## Documentation

Additional documentation lives in [`docs/`](docs/) — the user manual covers the voice library format, remap profiles and MONTAGE mode in depth.

---

*PSRstudio is an independent project and is not affiliated with or endorsed by Yamaha Corporation.*
