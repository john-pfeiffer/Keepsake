# Keepsake

**A granular sampler-to-synth. Capture a split-second moment from any sound and play it as an instrument.**

VST3 · AU · Standalone — JUCE 8, C++20, CMake.

Drag in any audio file, scrub to a moment, keep a 10–500 ms window of it, and play that
instant across the keyboard.

---

## Status

All milestones **M0–M6** of the Keepsake concept & build specification (v0.1 draft,
held outside this repo) are implemented. Section references in code comments —
"spec §2.2" and so on — point at that document.

| Milestone | Scope | State |
|---|---|---|
| **M0** Scaffold | CMake/JUCE 8, VST3 + AU + Standalone, resizable UI, CI, pluginval | Done |
| **M1** Capture | File import, zoomable waveform, draggable capture bracket, audition, capture embedded in state | Done |
| **M2** Cloud | Granular engine, all six grain params, 12-voice poly, amp envelope | Done |
| **M3** Tone | Root + cycle extraction, mipmapped wavetable, Frame, Place re-extraction | Done |
| **M4** Focus + Mod | Focus coupling, filter, ENV2, LFOs, mod matrix, mono/legato | Done |
| **M5** FX + Presets + Randomize | Warmth/Chorus/Air, preset browser, Randomize | Done |
| **M6** Package | macOS pkg (notarized when certs configured), Inno Setup installer, GitHub Releases | Done |

Focus carries the full §2.4 coupling: toward Tone the cloud condenses (density
up, grains shorter, shimmer down); toward Cloud, Tone ducks early and detunes
slightly. M4/M5 controls live in a stock tab strip (Amp/Filter/Env2/LFOs/Voice/
Mod/FX), with the preset bar — prev/next, name, save, Randomize, Back — across
the top.

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
- **M3** — "alias-free chromatic playback C1–C7": `tone_aliasFreeAcrossKeyboard_*`
  render octaves plus mip-boundary notes from a harmonic ("vocal-like") source with
  Root set correctly and from a seeded drum burst with an arbitrary Root, FFT the
  steady state (4-term Blackman-Harris), and fail on any off-harmonic-grid spur
  above −50 dB. A permanent negative control forces the full-bandwidth table at C7
  and asserts the detector fires — a broken analyzer cannot silently pass.
- **M3** — "Place automation sweeps without clicks":
  `processor_placeSweepWithReExtractionIsClickFree` steps Place and re-extracts ten
  times across a sweep, rendering through every set-swap crossfade.
- **M4** — "the one-knob morph feels musical" has a machine proxy:
  `processor_focusSweepIsClickFreeAndLevelStable` sweeps Focus end to end and holds
  every 100 ms RMS window inside a ±3 dB corridor (texture may change, level may
  not), and `coupling_curvesKeepTheirLoadBearingProperties` asserts the
  density×size reciprocal invariant that makes that possible. The human demo
  remains a human test.
- **M4** — modulation correctness: bit-identical renders across block sizes *with
  an LFO active* (`processor_blockSizeIndependentWithLfoModulationActive` — the
  control tick is anchored to the voice-local clock, not host blocks), click-free
  LFO sweeps on Cutoff/Frame/Focus/Pitch/Place, Loop vs Ping-Pong wrap proven
  distinct, mono holds one voice under a chord, legato provably skips the
  retrigger, glide measured mid-flight by FFT.
- **M5** — FX correctness: Warmth saturates (third harmonic +25 dB over a
  <−50 dB clean baseline) at level (±3 dB RMS, `fx_warmthAddsHarmonics…`);
  Chorus thickens at level; Air rings a decaying tail after the amp release and
  never guillotines it when the Mix knob closes (`fx_closingTheMixKnob…`);
  the per-voice Reverb Mix send — the M4 contract — is audible at Mix 0
  (`fx_perVoiceReverbSendIsAudibleAtMixZero`); the whole chain preserves
  bit-identical block-size independence; knob sweeps through zero are
  click-free.
- **M5** — presets and Randomize: save/load round-trips parameters *and* the
  embedded keepsake; a preset missing newer parameters loads them at defaults,
  not stale values (pins the JUCE `replaceState` behavior we rely on);
  prev/next cycle sorted names; Randomize rolls everything except master
  output, restores exactly on undo, and is safe mid-playback.

What is *not* covered here: the spec's manual matrix (Ableton, Logic, Reaper, FL Studio
at 44.1/48/96k) is a human step before release. Nothing in this repo has been loaded
into a real DAW yet — CI proves it builds and passes pluginval on all three platforms,
which is not the same thing.

---

## Installing

Grab the installer for your platform from the repository's **Releases** page
(built and validated by CI from the tagged commit):

- **macOS** — `Keepsake-<version>-macOS.pkg` installs the VST3, AU and the
  standalone app to their system locations. Release-candidate builds may be
  unsigned: if Gatekeeper objects, right-click the pkg and choose *Open*.
  Signed releases are notarized and open normally.
- **Windows** — `Keepsake-<version>-Windows-Setup.exe` installs the VST3 to
  `C:\Program Files\Common Files\VST3` and the standalone to Program Files.
  The installer is unsigned in v1; click through SmartScreen via
  *More info → Run anyway*. The uninstaller removes both.

Presets are saved to `Documents/Keepsake/Presets` — they are not touched by
install or uninstall, and each preset embeds its own capture audio.

To uninstall on macOS, delete `Keepsake.vst3` from
`/Library/Audio/Plug-Ins/VST3`, `Keepsake.component` from
`/Library/Audio/Plug-Ins/Components`, and `Keepsake.app` from Applications.

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
  FX bypassed (all FX at 0):
   1 voices: ~0.5 % of one core   (realtime factor ~200x)
   4 voices: ~2.0 % of one core   (realtime factor  ~50x)
   8 voices: ~4.0 % of one core   (realtime factor  ~25x)
  12 voices: ~5.9 % of one core   (realtime factor  ~17x)

  with FX (Warmth 50, Chorus 50, Air 30):
   8 voices: ~4.9 % of one core   (realtime factor  ~21x)
  12 voices: ~6.8 % of one core   (realtime factor  ~15x)
```

Measured at Focus 0.5 — Cloud, Tone, filter and mod matrix all active on every
voice; the FX rows add the whole master chain. Even 12 voices with FX sits
inside the spec's ~8% budget. Absolute numbers drift with the shared
container's day-to-day speed (the M4 README quoted ~3.0% for 8 voices; the M4
*code* measures ~3.9% on today's machine — same code, slower day), so compare
rows within one run, not across READMEs.

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
- **Presets** live in the top bar: type a name, *Save*; step with **< >** or pick
  from the list. A preset embeds its keepsake, so it plays anywhere. **Random**
  rolls every control except the output level (and the loaded audio); **Back**
  undoes it. Presets are stored in `Documents/Keepsake/Presets`.

Every control is an automatable host parameter, including Place and Keep Length — the
waveform bracket writes through the parameter system with proper begin/end gestures, so
dragging it records clean automation.

---

## Notes on the implementation

A few decisions worth knowing before editing:

- **Cloud reads the source buffer directly, not a copied capture buffer.** The spec
  calls for Place to be smooth and automatable on Cloud, so grains simply spawn from the
  current read region — there is no per-Place re-capture cost.
- **Tone re-extracts on a background worker with debounce + throttle.** A single
  dedicated thread rebuilds the wavetable set ~80 ms after Place/Root/Length/Frames
  stop changing, and at least every ~200 ms during sustained automation (a pure
  debounce would freeze Tone stale under a host LFO). Voices crossfade to a new set
  over 30 ms with a shared phase; extraction phase-normalizes every frame's
  fundamental (FFT bin-1 rotation), so the fade is linear, not equal-power.
- **Wavetable GC pins voice-held sets.** Voices hold set pointers across blocks (the
  fade, and a note's lifetime), so `WavetableStore::collectGarbage` skips anything a
  voice still references — wall-clock retention alone would free a set mid-note if
  the host suspends processing. Do not simplify this back to the SourceStore rule.
- **All parameter/mod evaluation happens on a 32-sample control tick anchored to
  the voice-local sample clock** — never to host block boundaries. That anchoring
  is what keeps renders bit-identical across block sizes even with LFOs running;
  a test enforces it, and a mutation check proves the test catches the
  block-anchored version.
- **Modulation combines in the destination's normalized domain** (skewed ranges
  get perceptually uniform mod for free); modulated values are never written back
  to the APVTS. Pitch is the one exception (additive semitones, ±12 st, feeding
  both engines). Place modulation affects Cloud only — Tone extraction follows the
  base parameter, by design (see the plan notes in the M4 commit).
- **Every AudioParameterChoice list is frozen ABI** — host automation stores
  index/(N−1), so changing any list's length silently remaps recorded automation.
  Mod source/dest, LFO shapes/divisions, filter types, voice modes: final.
- **ModWheel/aftertouch are captured at the synth level**, not per voice —
  juce::Synthesiser only dispatches controller events to voices that are already
  sounding, so a wheel moved before the note would otherwise be lost.
- **Air is a send-return, not an insert.** The reverb input is
  (main bus × Mix) + the per-voice wet-send bus, and the wet return is added on
  top — the dry signal is never ducked, and the Reverb Mix mod destination
  (a per-voice send level since M4) has a real bus to land in. Voices add their
  post-filter, post-envelope signal into the bus per sample through a smoothed
  send gain, same discipline as the blend gains.
- **FX activate on parameters, never on signal content.** Signal-gated
  activation would start an effect's internal smoothing ramps at a
  block-quantized position and break bit-identical block-size independence.
  Deactivation cools down instead of hard-cutting: the chorus lets its internal
  mix smoothing settle, the reverb rings out until its own output is silent
  (a fixed wall-clock tail would truncate big rooms), and both reset on the
  way out so no stale delay-line audio replays on re-entry. With every FX
  param at default the chain is exactly bypassed — the pre-M5 bit-identity
  tests still hold.
- **juce::Reverb's gains are smoothed from demo defaults** (dry 0.4!) —
  parameters must be applied *before* `setSampleRate`, whose reset snaps the
  smoothers. Otherwise the first engaged block leaks a dry-gain ramp.
- **Preset files are the state blob, byte for byte.** Same validation, same
  embedded audio. Missing parameters (older presets, M4-era sessions) load at
  defaults — that is JUCE 8 `replaceState` behavior, pinned by
  `presets_missingParametersLoadAsDefaultsNotStaleValues`; if a JUCE upgrade
  changes it, defaults must be merged into the tree in `setStateInformation`.
- **Randomize excludes exactly one parameter: master output** (spec's ear
  rule); the loaded audio is not a parameter. It writes through
  `setValueNotifyingHost` with gestures so hosts record it, and stashes one
  level of undo.
- **Mip selection is safe-side:** continuous level `log2(inc) + 1`, so both blended
  levels stay inside the alias limit at every pitch. Plain `log2(inc)` blends a
  floor level that keeps twice the safe harmonic count — measurably aliased at the
  top of the keyboard (this was caught by the alias test, not by ear).
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

## Plugin identity — frozen at 1.0.0

| Field | Value |
|---|---|
| `COMPANY_NAME` | `Elan Vital Studios` |
| `BUNDLE_ID` | `com.elanvitalstudios.keepsake` |
| `PLUGIN_MANUFACTURER_CODE` | `EVSR` |
| `PLUGIN_CODE` | `KPSK` |

These are load-bearing: the AU manufacturer/plugin code pair and the bundle ID are
what saved DAW projects reference, so they must never change after release. (DAW
sessions saved against pre-1.0 dev builds — bundle id `com.evs.keepsake` — will
re-scan the plugin as new; that is the one-time cost of finalizing, paid before
anything shipped.)

Still open before public distribution, both outside this repo: the JUCE licence
tier decision (AGPL vs commercial, against repo visibility), and the spec's manual
DAW matrix — Ableton, Logic, Reaper, FL Studio at 44.1/48/96 kHz — which gates the
plain `v1.0.0` tag. Release candidates (`v1.0.0-rc.N`) can ship installers before
that.
