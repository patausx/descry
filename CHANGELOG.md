# changelog

all notable changes to descry. dates are release dates.

the format is loosely [keep a changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [semver](https://semver.org/spec/v2.0.0.html) as far as an
end-user application meaningfully can.

each release also has a [GitHub release page](https://github.com/patausx/descry/releases)
with binaries, checksums and the long-form notes.

## [unreleased]

## [1.0.7] — 2026-08-20

### added
- **one-pass dry stem export** — `ZL+SELECT` in Project writes a synchronized
  reference mix plus every arranged track as `NAME_mix.wav` / `NAME_t1.wav`…`t8.wav`.
  stems retain track DSP, fader, pan and sidechain duck but omit global delay,
  reverb and master processing for clean DAW mixing; empty tracks are skipped
- **precision sampler viewports** — Circle Pad up/down zooms both WAVE and SLICE
  waveforms up to 256× and left/right pans them; crop and slice markers, touch
  dragging and live playheads all follow the zoomed frame range without changing
  playback parameters
- **faster WAV audition** — LOAD-panel `X` preview decodes a bounded two-second
  audition instead of waiting for five seconds of long source material; full
  `A` imports keep the existing 15-second capacity

### fixed
- **sampler panels no longer hijack TYPE edits** — with the cursor on `TYPE`,
  `A/B/X/Y` changes the instrument engine directly from WAVE/SLICE/LOAD instead
  of firing crop/slice/browser actions or forcing a return to KB
- the in-app manual now opens only from the visible `?` button instead of an
  invisible hit target spanning the entire hint strip
- mixer faders on both screens use restrained theme-derived depth bands and a
  narrow highlight instead of dead flat single-colour fills
- sampler `CROP` no longer overflows its position math on multi-second samples
  and keeps exact stereo frame boundaries instead of returning one random hit —
  reported by **@HexManic** in [#7]; thanks for the clear report and zoom request
- offline export now sequences with tick-accurate sub-chunks and renders from an
  immutable project snapshot; coherent stem takes are published transactionally
  and partial/failed sets are removed

## [1.0.6] — 2026-08-15

### added
- **Phrase Tools and a permanent Phrase inspector** — rotate, reverse,
  transpose/octave, velocity ramps and deterministic generators can target the
  playable phrase or a selection. the inspector resolves the active instrument,
  source, live envelope, ALWAYS defaults/mod table and all three decoded FX; use
  SELECT on the instrument column to jump into its editor
- **generative Phrase Tools** — Euclidean rhythms, density gating, humanize,
  ratchets, scale-aware mutation, random notes, trigger-chance spread and `EVN`
  cycle distribution, alongside the existing transforms. every generator takes
  an explicit seed, so the same phrase + settings reproduce the same bars; they
  stay inside the selection, respect the active key/scale, never clobber
  unrelated FX, and each result is a single undo
- **IRONLUNG flagship demo** — a copyright-clean 174 BPM jungle track with a
  procedural two-bar break sliced into 32 chromatic chops, rearranged with rolls
  and reverse fills, plus reese bass, sub, pads, stabs and sidechain ducking. the
  release bundle includes both the project and its required sample audio
- **analog editing controls** — the circle pad accelerates movement through the
  Song view and scrubs/zooms the sampler window; the C-stick acts as a relative
  fine/coarse value encoder. this replaces the old stick-as-KAOSS and
  right-stick sends/crush mappings; the KAOSS `STK` toggle is gone
- **continuous integration** — every push and pull request runs the host test
  suite, builds the IRONLUNG bundle and performs a full devkitARM build; both the
  demo and `descry.3dsx` are uploaded as artifacts
- this changelog

### fixed
- **song rows no longer desynchronize tracks** — every Song row now has one
  shared boundary derived from its longest chain. shorter chains loop inside the
  row, `EMPTY` cells wait silently, and `song_wrapped()` can no longer fire early
  from an unused track and truncate an offline render
- **WAV export now renders the complete mix and arrangement** — channel faders,
  master volume, mutes, delay/reverb and sidechain ducking match live playback;
  long rests survive, songs run to their shared end instead of a premature track
  boundary or 60-second cutoff, and a roughly three-second FX tail is preserved
- track mutes persist with the project; pressing PLAY and leaving solo mode no
  longer discard them
- phrase and chain playheads follow the track and position actually sounding,
  including phrase boundaries and non-zero tracks, instead of reading one step
  ahead or assuming track 0
- whole-instrument edits, preset loads, type switches and clones are undoable;
  loading or replacing a project clears old history instead of letting undo
  splice data from the previous project
- **IRONLUNG no longer collides with the optional BMT pack** — its procedural
  break owns sample slot 63; the BMT add-on is explicitly limited to slots 32–62
- **the unsaved-changes flag no longer lies** — switching themes, changing the
  scope style, opening a panel or jamming over a saved song marked the *project*
  dirty, so descry asked to save work you never changed. project state and view
  state are now separate; edits that hit a value limit don't mark anything
  either, because they didn't change anything
- **overlays no longer leak the press that closes them** — help, theme picker,
  FX list, Phrase Tools and the KAOSS assign menu closed on the same frame they
  were dismissed, letting that button also act on the screen underneath. they
  now animate out and stay input-opaque while they do
- leaving KAOSS mode during a held gesture now performs the normal release ramp
  instead of leaving performance parameters frozen
- grid lines and inactive borders were drawn brighter than the dim text they
  frame in every theme, so the scaffolding read louder than the notes
- touch keys, pads, filter tints and confirmation states no longer stay in the
  cretaceous palette after switching themes
- arrangement edits now set the unsaved flag; mute/solo state, the Song end and
  the active playhead owner are visible where they matter instead of being
  hidden in another screen
- the Instrument type table no longer advertises the nonexistent `GRAN` engine,
  and clean builds are warning-free

### changed
- START in the Song view plays from the cursor row; hold L+START to play from
  the top
- Song exports stream transactionally to the SD card with a ten-minute safety
  cap: a failed/full-card write cannot replace a good render with a torn WAV
- `DTIM` is displayed in milliseconds instead of exposing raw 32-frame storage
  units
- undo/redo can report every phrase step a grouped edit restored, so undoing a
  Phrase Tools transform acknowledges all the rows it touched instead of one
- the UI is split into per-screen translation units (`song`, `chain`, `phrase`,
  `phrase_tools`, `history`, `chrome`, `scope`, `theme_menu`); `app.cpp` keeps
  only `update()` and `tick()`

## [1.0.5] — 2026-08-03

### added
- **complete sampler capture / slicing workflow** — recording, trimming,
  transient and equal chopping, per-slice reverse, slice-to-phrase and
  slice-to-kit, beat-sync repitch, and a WAV browser handling 8/16/24/32-bit and
  float files. SD and decode work happens off the audio lock, then swaps under it
- **capture any sampler window as a permanent oscillator** — the visible window
  becomes a single-cycle USER wavetable plus a matching wavsynth instrument, DC
  removed and normalised, persisted across reboots
- song exports go to `renders/` named after the project, never overwriting an
  existing take (`NAME_01.wav`, `NAME_02.wav`, …). hold **R** in the PROJECT view
  to rename; the view shows the target path before you press SELECT ([#6])
- `make tests` builds and runs the host-side suite

### fixed
- two use-after-free races between the UI and the audio thread, found by an
  external audit of 1.0.4
- exports rendered a different mix than you heard, and stopped short of the song
- track mutes were not persisted, and PLAY or un-solo wiped them
- a deep down-transpose under `DLY` jumped ten octaves up (unsigned wrap before
  the clamp: −1 became 255 became 127)
- full-scale peaks read as silence on the meters (negating `INT16_MIN` wraps)
- cross-core flags were `volatile` rather than atomic, and a failed audio init
  leaked everything it had already acquired
- the in-app help page documenting the SD layout was drawn off-screen

### changed
- the SD layout no longer has `render.wav`; it is `renders/`. a leftover file is
  unused and can be deleted

## [1.0.4] — 2026-07-15

### added
- **explicit record modes** ([#5]) — the touch keyboard used to *always* write
  notes. REC now cycles **JAM** (preview only, the new default), **WRT** (notes
  land at the cursor) and **LIVE** (record onto the playing step), with a **CLR**
  button beside it. touch-entered notes participate in undo
- **scope styles** — WAVE, BARS and **X-Y**: a mid/side goniometer with
  auto-gain, phosphor-decay trail and beam blanking on transients

### fixed
- the X-Y scope exhausted the citro2d object pool and flickered the bottom screen

## [1.0.3] — 2026-07-11

### added
- **in-app manual** ([#3]) — an 8-page guide on the console, behind the `?`
  badge. the FX pages are generated from the engine's own command table, so the
  help cannot drift from what the player executes
- **INIT preset** in every synth engine — a true blank patch to build from
- **DSN drum presets** — KICK / SNARE / HAT / TOM
- **reverb size + damping** (`RSIZ` / `RDMP`) in the mixer master strip, saved
  per project
- tap the KEY readout: left half cycles the root, right half the scale
- loading over unsaved changes asks for confirmation

### fixed
- `FWDLOOP` / `REVLOOP` with no loop markers now loop the play window (M8
  behaviour) instead of silently doing nothing
- **audio cutout recovery** — a watchdog unpauses the NDSP channel after ~200 ms
  of unexpected silence and re-initialises it after ~600 ms
- the phrase R-hint bar overflowed the bottom screen

## [1.0.2] — 2026-07-08

### added
- instrument voices refresh live while you edit synth parameters
- animated live envelope overlay for wavsynth, sampler, FM and DSN
- horizontal song timeline mode

### fixed
- an explanatory error screen when NDSP init fails because `dspfirm.cdc` is
  missing, instead of exiting instantly with no reason given ([#2])
- exit/autosave skips unchanged sample writes
- bottom panel hit zones no longer swallow upper navigation touches

## [1.0.1] — 2026-07-07

driven almost entirely by [#1].

### fixed
- **tempo jitter** — sequencer events were quantised to audio-buffer boundaries,
  up to 32 ms off-grid and audible as unstable tempo. the scheduler is now
  tick-accurate inside the buffer
- KAOSS pad parameters were stomped by every note trigger; gestures now own their
  parameters while held and hand them back on the release ramp
- mixer settings apply on project load, not on the first visit to the mixer
- the R-modifier hint bar told the wrong story on the table screen
- tapping REC in the drumkit GEN panel did nothing

### added
- **L + A** in the instrument view clones the instrument to the first free slot
- **R + Y** in the phrase view clears the whole phrase (undo-tracked)
- KAOSS send curves are perceptual; `M>C` engages an LPF and gives the wobble
  headroom
- the instrument view shows a `USED IN N PHRASES` counter — instruments are a
  global bank, and the UI now says so
- the full user guide, [docs/GUIDE.md](docs/GUIDE.md)

## [1.0.0] — 2026-07-06

first public release. song/chain/phrase sequencer, 23 FX commands, mod tables,
five synth engines (wavsynth / 4-op FM / sampler / drumkit / DSN analog voice),
per-track DSP, delay + reverb, KAOSS-style XY pad, mic sampling, user
wavetables, 6 colour themes. New 3DS / New 2DS only.

[unreleased]: https://github.com/patausx/descry/compare/v1.0.7...HEAD
[1.0.7]: https://github.com/patausx/descry/compare/v1.0.6...v1.0.7
[1.0.6]: https://github.com/patausx/descry/compare/v1.0.5...v1.0.6
[1.0.5]: https://github.com/patausx/descry/compare/v1.0.4...v1.0.5
[1.0.4]: https://github.com/patausx/descry/compare/v1.0.3...v1.0.4
[1.0.3]: https://github.com/patausx/descry/compare/v1.0.2...v1.0.3
[1.0.2]: https://github.com/patausx/descry/compare/v1.0.1...v1.0.2
[1.0.1]: https://github.com/patausx/descry/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/patausx/descry/releases/tag/v1.0.0
[#1]: https://github.com/patausx/descry/issues/1
[#2]: https://github.com/patausx/descry/issues/2
[#3]: https://github.com/patausx/descry/issues/3
[#5]: https://github.com/patausx/descry/issues/5
[#6]: https://github.com/patausx/descry/issues/6
[#7]: https://github.com/patausx/descry/issues/7
