# Keepsake

**A granular sampler-to-synth. Capture a split-second moment from any sound and play it as an instrument.**

VST3 · AU · Standalone — JUCE 8, C++20, CMake.

Drag in any audio file, scrub to a moment, keep a 10–500 ms window of it, and play that
instant across the keyboard.

---

## Status

Milestones **M0–M2** of the Keepsake concept & build specification (v0.1 draft, held
outside this repo) are implemented. Section references in code comments — "spec §2.2"
and so on — point at that document.

| Milestone | Scope | State |
|---|---|---|
| **M0** Scaffold | CMake/JUCE 8, VST3 + AU + Standalone, resizable UI, CI, pluginval | Done |
| **M1** Capture | File import, zoomable waveform, draggable capture bracket, audition, capture embedded in state | Done |
| **M2** Cloud | Granular engine, all six grain params, 12-voice poly, amp envelope | Done |
| **M3** Tone | Root + cycle extraction, mipmapped wavetable, Frame, Place re-extraction | Not started |
| **M4** Focus + Mod | Focus macro, filter, ENV2, LFOs, mod matrix | Not started |
| **M5** FX + Presets + Randomize | Warmth/Chorus/Air, preset browser, Randomize | Not started |
| **M6** Package | Notarized macOS pkg, Inno Setup installer, GitHub Releases | Not started |

The UI reserves the space that Focus and Tone will occupy and says so on screen, rather
than showing controls that do nothing.

### Milestone exit tests

The spec defines each milestone by an exit test. Where those are machine-checkable they
are tests in `tests/`, not claims:

- **M1** — "reload project, keepsake intact with no source file present":
  `processor_stateRoundTripPreservesParametersAndAudio` saves state, loads it into a
  fresh processor, and asserts the audio came back. Nothing touches the filesystem.
- **M2** — "no clicks at any param extreme": `grains_noClicksAtParameterExtremes`
  renders 15 extreme settings and fails on any sample-to-sample step above 0.25
  (the 220 Hz full-scale test tone's own maximum step is ~0.03). Each case must also
  prove it rendered audio, so silence cannot pass the click check.
- **M2** — "CPU < ~8% for 8 voices": measured, not assumed. See Benchmark below.
- **M2** — "12-voice poly": `processor_playsTwelveVoicesAtOnce`.

What is *not* covered here: the spec's manual matrix (Ableton, Logic, Reaper, FL Studio
at 44.1/48/96k) is a human step before release. Nothing in this repo has been loaded
into a real DAW yet — CI proves it builds and passes pluginval on all three platforms,
which is not the same thing.

---

## Building

Requires CMake ≥ 3.22 and a C++20 compiler. JUCE 8.0.4 is fetched automatically.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To iterate against a local JUCE checkout instead of re-fetching, put one at `libs/JUCE`
and CMake will prefer it.

On Linux you will need the usual JUCE dependencies:

```bash
sudo apt-get install -y libasound2-dev libjack-jackd2-dev libfreetype6-dev \
  libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev
```

### Tests

```bash
ctest --test-dir build --output-on-failure
```

### Benchmark

The M2 CPU budget is a real number, so it is measured rather than asserted — wall-clock
thresholds on shared CI runners produce flaky failures that train people to ignore the
suite. CI prints it on every run; read it there or locally:

```bash
./build/KeepsakeTests --bench
```

Reference figures (Release, 48 kHz, 256-sample blocks, one core of a 4-core cloud
container — a slower machine than the spec's "modern laptop"):

```
   1 voices: 0.38 % of one core   (realtime factor 265.1x)
   4 voices: 1.59 % of one core   (realtime factor  62.8x)
   8 voices: 3.13 % of one core   (realtime factor  32.0x)
  12 voices: 4.91 % of one core   (realtime factor  20.4x)
```

8 voices at 3.13% is inside the spec's ~8% budget.

---

## Using it

- **Drop an audio file** onto the window, or press *Load...*. WAV/AIFF/FLAC/MP3/Ogg.
  The file is resampled to the host rate on import.
- **Drag the warm bracket** on the waveform to choose the moment.
  - Drag the middle → **Place**
  - Drag either edge → **Keep Length**
  - Click outside the bracket → jump there
  - Scroll → zoom about the pointer; shift+scroll → pan; double-click → zoom to bracket
- **Play file / Play keep** audition the source and the captured window.
- Play MIDI. **Root** tells the engine what pitch the source material is; everything
  tracks from there.

Every control is an automatable host parameter, including Place and Keep Length — the
waveform bracket writes through the parameter system with proper begin/end gestures, so
dragging it records clean automation.

---

## Notes on the implementation

A few decisions worth knowing before editing:

- **Cloud reads the source buffer directly, not a copied capture buffer.** The spec
  calls for Place to be smooth and automatable on Cloud, so grains simply spawn from the
  current read region — there is no per-Place re-capture cost. (Tone will need the
  copied window and background re-extraction; that is an M3 concern.)
- **Source handover is lock-free on the audio side.** The audio thread reads a plain
  atomic pointer; replaced sources are parked and only freed after two seconds. Do not
  "optimise" that into freeing as soon as the refcount drops — the delay is what makes
  the raw pointer safe.
- **Grain level is overlap-compensated** by 1/sqrt(overlap). Without it, sweeping Density
  from 2/s to 200/s is roughly a 20 dB jump.
- **Plugin state is binary with a magic/version/length header.** Hosts (and pluginval)
  hand `setStateInformation` arbitrary bytes; the header makes truncated and garbage
  input a clean rejection instead of a partially-applied preset. The version field is
  the migration hook when the format changes.
- **String literals are ASCII-only.** `juce::String(const char*)` cannot infer an
  encoding and asserts on bytes above 127. Source files are UTF-8 (comments cite spec
  sections) and MSVC is told so via `/utf-8`.

---

## Compatibility

- **macOS 11+** (universal, Apple Silicon + Intel) and **Windows 10+** (x64). Both are
  built and pluginval-tested in CI on every push; neither is a port of the other. Linux
  also builds in CI as a fast portability check, but is not a shipping target.
- **DAWs:** VST3 + AU covers Ableton Live, Logic, FL Studio, Reaper, Studio One,
  Cubase/Nuendo, Bitwig, Cakewalk and others.
- **Pro Tools is not supported.** It requires AAX, which needs an Avid developer
  agreement and PACE/iLok signing. Deferred until there is demand.
- **Automation:** every user-facing control is in the `AudioProcessorValueTreeState` and
  therefore host-automatable from v1, with units and value-to-text formatting so the
  parameters read cleanly in automation lanes. There are no hidden or UI-only
  parameters. (MIDI CC learn is a possible v2 nicety; host automation is day one.)

---

## Plugin identity — needs a decision before release

`CMakeLists.txt` currently carries **placeholders**:

| Field | Placeholder |
|---|---|
| `COMPANY_NAME` | `EVS` |
| `BUNDLE_ID` | `com.evs.keepsake` |
| `PLUGIN_MANUFACTURER_CODE` | `Evsy` |
| `PLUGIN_CODE` | `Kpsk` |

The spec lists plugin identity metadata (manufacturer name/codes, bundle ID — shared with
Folie) as a pre-M0 administrative item held outside the spec, along with the JUCE licence
tier check against repo visibility. These need real values before anything ships: the AU
manufacturer/plugin code pair must be stable, because changing it after release breaks
every saved project that references the plugin.
