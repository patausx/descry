# descry user guide

a music tracker + synthesizer for the new nintendo 3ds.
this guide covers everything: the data model, every screen, every instrument
engine, the full FX command set, performance tools, sampling, and the SD layout.

descry needs a **new** 3ds/2ds. everything below assumes you launched
`descry.3dsx` from the homebrew launcher (or the .cia from the home menu).

> **in-app manual:** tap the **`?` badge** (or anywhere on the hotkey hint
> strip) on the bottom screen — an 8-page compressed version of this guide
> opens right on the console: basics, global keys, phrase editing,
> instruments, the full FX list, performance and sampling. d-pad or tap to
> flip pages, B to close.

---

## 1. first beat in five minutes

a hands-on quickstart. exact buttons, no theory.

1. **make a drumkit.** press L or R until the header shows the **instrument**
   view. cursor starts on the `TYPE` row — press A a few times until it reads
   `DRUMKIT`. the bottom screen now shows two tabs: `KB` and `GEN`.
2. **generate sounds.** tap the `GEN` tab on the touchscreen. the pad under the
   cursor is the target ("GEN > PAD 0"). tap `KICK` — a kick is synthesized into
   a sample slot and assigned to that pad. move the cursor with the d-pad to
   another pad, tap `SNARE`, then `CLHAT`. no files needed.
3. **write a phrase.** press L/R until you reach the **phrase** view. the
   cursor sits on the note column of step 0. press A — a note appears (snapped
   to the song key) and the instrument column auto-fills with your drumkit
   slot. move down 4 steps, press A again. put hats on every other step. A/B
   nudge the note ±1, X/Y jump ±12 (octave). you can also tap the touch
   keyboard/pads to enter notes — tap the `REC` button until it reads `WRT`
   (write mode) first; in the default `JAM` mode keys only preview.
4. **put the phrase in a chain.** go to the **chain** view. cursor on row 0,
   press A — the row now points at phrase `00`.
5. **put the chain in the song.** go to the **song** view. cursor on row 0 of
   track 0, press A — the cell points at chain `00`.
6. **press START.** the song plays. press START again to stop. that's the whole
   loop: song → chain → phrase → notes.

---

## 2. how descry thinks

the hierarchy is the classic lsdj/m8 one:

| level | size | contains |
| --- | --- | --- |
| song | 256 rows × 8 tracks | each cell = chain index or empty |
| chain | 16 rows | each row = phrase index + per-row transpose |
| phrase | 16 steps | note · instrument · velocity · 3 FX columns |

- everything is **hex**. chain `0A` is chain ten. `FF`/`--` = empty.
- a phrase has a playable **length 1..16** (ZL+up/down in the phrase view).
  a 12-step phrase against a 16-step one drifts accents every bar — free
  polymetry.
- 256 chains, 256 phrases, 32 mod tables, 128 instruments per project.

### instruments are a global bank

instruments live in **slots 00..7F**, shared by the entire project. the `I`
column of a phrase step is just a reference to a slot.

**warning:** editing instrument `00` changes *every* phrase that plays `I=00`,
on every track, everywhere in the song. if you want a variant — a brighter
copy, a detuned copy — do **not** tweak the original. instead:

- **hold L + press A** in the instrument view — clones the current instrument
  into the first free slot and jumps there (you'll see a `CLONE xx` toast).
- **hold L + left/right** in the instrument view — steps through instrument
  slots.

the same "shared until you clone" logic applies to phrases and chains — see
ZL+SELECT clone in the screen sections below.

---

## 3. the screens

tap L / R to cycle: **song → chain → phrase → instrument → table → mixer →
project**. you can also tap the screen tabs strip on the touchscreen. the top
screen is the data grid; the bottom screen is the touch keyboard, pads, panels
and the hint bar (hold ZL, L or R to see that modifier's combo map live).

### song

256 rows × 8 tracks of chain slots.

| key | action |
| --- | --- |
| d-pad | move |
| A / B | chain index +1 / −1 (A on empty creates `00`) |
| Y | queue the chain under the cursor on its track (live mode) |
| X | queue a stop for the track |
| SELECT | open the chain under the cursor |
| ZL+SELECT | **deep clone** the chain — copies the chain and all its phrases |
| touch pads (bottom) | per-track **solo** toggle |

**reading the grid**

| you see | it means |
| --- | --- |
| `END` + a line across the row | the last row with any chain. playback loops here; rows below are shaded and never play until you put a chain in one |
| `M` next to a track label, column washed out | that track is **muted** (mixer / touch header, saved with the project) |
| `S` next to a label | that track is **soloed** |
| `s`, column dimmed | silenced only because another track is soloed — un-solo brings it back |

mute is deliberately visible here and not just in the mixer: this is the screen
you stare at, so "why is this silent" gets answered without leaving it.

### chain

16 rows of phrase + transpose.

| key | action |
| --- | --- |
| A / B | edit phrase index / transpose (cursor column) |
| SELECT | open the phrase under the cursor |
| ZL+SELECT | clone the phrase into a free slot and point this row at the copy |

while the transport runs, `▶T#` in the header tells you which track's playhead
you are watching (a chain can be used by several tracks at once).

### phrase

16 steps × 9 columns: note · inst · vel · fx1 cmd/val · fx2 · fx3.

| key | action |
| --- | --- |
| A / B | value ±1 (notes walk in-scale when a key is set) |
| X / Y | value ±12 (notes) / big step |
| SELECT | on the note column: preview. on the **instrument column: open the instrument editor**. on an FX cmd column: **FX command list** |
| ZL+X / ZL+Y | copy / paste step (or block) |
| ZL+up/down | phrase length 1..16 |
| ZL+SELECT | **selection mode** — cursor extends a range; A = copy, X = cut, B = cancel |
| ZL+B / ZL+A | undo / redo |
| R+A | clear the cell under the cursor |
| R+B | clear the whole step |
| R+Y | clear the entire phrase |
| R+X | open **Phrase Tools**; uses the active selection, otherwise playable phrase |
| L+left/right | switch to another phrase slot |

**Phrase Tools:** rotate up/down, reverse, transpose ±1/±12, velocity ±8,
velocity ramps, plus deterministic generators: Euclidean rhythm, density gate,
velocity/timing humanize, ratchet fill, scale-aware note mutation/randomization,
trigger-chance spread and `EVN` cycle distribution. Generators open a parameter page:
left/right changes amount, X/Y jumps ±8, up/down changes the hexadecimal seed,
and A applies. The same seed and settings reproduce the same result. Operations use
the active key/scale, never touch rows outside the selection, and never overwrite
unrelated FX when all three lanes are occupied. Every transform is a single
undo/redo operation.

the playhead follows **whatever track is playing this phrase** — not just track 0
— and `▶T#` at the right edge of the grid names it. if two tracks share the
phrase, the one under your song-view cursor wins.

**the side panel.** the right third of the screen is a permanent inspector for
the step under the cursor, so you never have to leave the view to find out what
`I05` actually is:

- **instrument** — id, engine type, MONO/POLY and name. if the step's instrument
  column is empty, the panel resolves it the way the engine does (the last
  instrument declared above it) and marks the inherited value with `^`.
  **SELECT on the instrument column opens that instrument's editor.**
- **source** — what the engine actually plays: the sample slot + name, the
  wavesynth shape, the FM algorithm, or the DSN oscillator pair.
- **envelope** — the real ADSR of that instrument, drawn from its own params
  (FM shows its loudest carrier operator; a DrumKit reports its pad count
  instead, since its pads are one-shots). a dot rides the curve while a voice of
  that instrument is actually sounding.
- **ALWAYS** — the instrument's FX defaults and mod table. these are applied to
  the track *before* every step's own FX and are otherwise invisible here, which
  is the usual answer to "why does this instrument sound filtered". only
  non-default values are listed, so a clean instrument prints nothing.
- **FX** — all three slots decoded: mnemonic, hex value, the typed widget and
  the full english name. the slot under the cursor is highlighted.

### instrument

editor for the slot shown in the header. layout depends on the engine (see
section 4). common keys:

| key | action |
| --- | --- |
| d-pad | move between params (grid layouts wrap) |
| A / B / X / Y | value ±1 / ±16 |
| L+left/right | switch instrument slot |
| L+A | clone instrument to a free slot |
| ZL+SELECT | focus the **FX defaults** strip (see 5) |

### table

mod tables: 3 FX lanes × 16 rows, looped while a note is alive.

| key | action |
| --- | --- |
| A / B / X / Y | edit cmd / value |
| R+A / R+B | table **speed** (ticks per row, 1..16, shown as `SPD`) |
| L+left/right | switch table slot |
| SELECT | assign this table to the current instrument + preview |

### mixer

8 channel strips + a master strip + a groove zone (see 6 for groove).

| key | action |
| --- | --- |
| left/right | move across strips |
| up/down | track strip: fader / duck depth. master strip: MST, DTIM, DFB, DWET, RWET, RSIZ, RDMP, DUCK, DREL |
| A / B / X / Y | value ±1 / ±16 |
| SELECT | mute toggle on a track strip (saved with the project) |

the bottom screen shows 9 **touch faders** (8 tracks + MST): drag = volume,
tap the header = mute. `DUCK` selects the sidechain source track (the track
whose notes pump the duck), `DREL` its release, and each track's second row
sets how deep that track ducks.

**signal flow:** `DTIM / DFB / DWET / RWET` are **bus** controls. the delay
and reverb buses only carry what tracks *send* to them — an instrument's
FX-defaults strip (`DEL` / `REV` values), the `SND` command in a phrase, or
the kaoss pad's DEL/REV destinations. if nothing sends, the wet knobs turn
silence up and down. same idea for the duck: it needs a source track *and*
non-zero duck depth on the tracks that should dip.

`DTIM` is shown in milliseconds. Internally the project stores 32-frame
quanta; at descry's fixed 32 kHz rate one quantum is exactly 1 ms. It is a free
delay time, not tempo-sync.

`RSIZ / RDMP` shape the reverb itself: `RSIZ` = room size (comb feedback —
bigger = longer tail), `RDMP` = damping (higher = darker tail). they apply
globally and save with the project.

### project

16 save slots with names.

| key | action |
| --- | --- |
| A | load slot (asks to confirm if you have unsaved changes) |
| Y | save to slot |
| X | new project |
| B | delete (press twice to confirm) |
| hold R | **rename mode**: A/B cycle the character, left/right move, X clear |
| SELECT | **render the song to WAV** |

the render lands in `renders/` named after the current project, and the view
shows the exact target filename (`SEL -> renders/NAME.wav`) *before* you press
SELECT — so rename first (hold R), then render. an existing take is never
overwritten: repeat renders become `NAME_01.wav`, `NAME_02.wav`, … which means
you can bounce several projects in one sitting and pull them all off the SD card
later. characters the FAT filesystem dislikes are stripped, spaces become `_`.

the export is the **same mix you hear**: channel faders, master volume, mutes,
delay/reverb settings and the sidechain duck all go into the file. it runs until
the song has played through once, then keeps rendering ~3 s so delay and reverb
tails ring out instead of being cut off. rests inside the song are fine — only a
project that never makes a sound at all is reported as a failed render.

autosave writes `session.tr3d` on exit.

### everywhere

| key | action |
| --- | --- |
| START | play / stop · SELECT+START = exit |
| START (song view) | plays from the **cursor row** — L+START starts from the top |
| L+d-pad | BPM (up/down ±1, left/right ±10) |
| R+up/down | groove (ticks per step) · R+left/right swing |
| L+SELECT | fullscreen oscilloscope |
| R+SELECT | screenshot (both screens, BMP → SD) |
| ZR (hold) | mic record |
| tap DESCRY logo | theme picker (6 themes) + scope style (WAVE / BARS / X-Y goniometer) |
| tap BPM readout | tap tempo |
| tap KEY readout | left half: cycle root · right half: cycle scale |

**the header** is always on and always tells you: which screen you're on and its
index, transport, BPM / groove, the mic record target, clock and battery — plus a
small breathing **dot** left of the clock whenever the project has unsaved
changes. no need to visit the project screen to check.

---

## 4. instruments

the `TYPE` row cycles: none → wavsynth → sampler → drumkit → fm → dsn.
every engine has a preset row/field — presets load values *and* rename the
instrument, then you tweak from there. **preset 1 is always `INIT`** — a true
blank patch for building sounds from scratch (the other presets are finished
timbres, not starting points). all engines share `TABLE` (mod table
slot) and `POLY` (mono = new note releases the old, poly = up to 4 voices
stack).

### wavsynth

classic waves with unison/detune. rows: `TYPE / OSC/SL / ATTACK / DECAY /
SUSTN / RELS / TABLE / POLY`. the `OSC/SL` row is one big knob over the whole
palette: first the 17 built-in presets —

`INIT · SAW LEAD · SUB BASS · PLUCK · PAD · ORGAN · STAB · BASS · BELL · CHIP ·
SWEEP · NOISE · DRONE · WARM PAD · GLASS PAD · DEEP DRONE · SHIMMER`

— then your **user wavetables**. drop single-cycle WAVs into
`sdmc:/3ds/descry/wavetable/` (16 slots, scanned alphabetically, resampled to
1024 points); scrolling past the built-ins switches the oscillator to the USER
shape and picks a table, keeping your envelope intact.

### fm

4 operators, 8 algorithms, feedback, per-op waveform + ADSR. top fields:
`TYPE / ALGO / FB / MAST / TABLE / PRESET`, then a 4-row op grid with columns
`RAT / WAV / LVL / ATK / DEC / SUS / REL` (up/down switch op, left/right switch
column). the algorithm diagram is drawn live on screen. op ratios come from a
fixed table (0.5, 1, 1.5, 2, 2.5, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16); waves
are SIN/TRI/SAW/SQR; feedback 0..7 on op1.

presets: `INIT · EP · BASS · LEAD · BELL · BRASS · PLUCK · ORGAN · PAD · STAB ·
WOOD · WAH · ICE`. `INIT` is DX7-style init voice: one plain sine carrier,
ops 2-4 silent.

### sampler

two-column param list: `TYPE / SAMPLE / PLAY / SLICE / SYNC / START / LENGTH` and
`LOOP-S / LOOP-E / DETUNE / ATK / REL / TABLE`.

- `PLAY` modes: `FWD · REV · FWDLOOP · REVLOOP · REPITCH · THRU`. loop modes
  use the `LOOP-S`/`LOOP-E` markers when set; with no markers they loop the
  whole play window (`START`..`LEN`). `THRU` is the break-chopper mode: a
  slice starts at its marker but plays through to the end of the sample
  instead of cutting at the next marker (let a crash ring out).
- `SLICE`: 0 = whole sample (scroll below 0 to toggle chromatic-slice mode),
  1..32 = play a fixed chop. slices are numbered left-to-right (sorted by
  position, not by creation order).
- `SYNC`: beat sync, akai/amiga style **repitch** (not time-stretch). `OFF` =
  play at root pitch. `1..8` = "this sample is N bars long" — the playback
  rate is scaled so it lasts exactly N bars at the project tempo, and the row
  shows the resulting shift (`2BAR -3.1st`). drop a 174bpm break into a 140bpm
  project, set `SYNC 1`, done — it fits, pitched down like real hardware.
  changing the song tempo re-fits automatically on the next note.
- `DETUNE` = fine tune ±50 cents.

the bottom screen has five tabs: **KB / WAVE / SLICE / LOAD / REC**.

- **WAVE** — sample editor. op buttons: `NORM · REV · FAD< · FAD> · G+3 ·
  G-3 · CROP · COPY · >WT`, drag the start/end markers on the waveform, A/B set
  the root note. `X` or `>WT` converts the visible window into a normalized,
  DC-free persistent USER wavetable and creates a Wavsynth instrument using it.
- **SLICE** — the break chopper. up to 32 markers per sample.
  - buttons: `TRNS` = transient auto-chop (press again to cycle LO/MID/HI
    sensitivity), `EQ n` = equal slice (press again to cycle 4/8/16/32),
    `REV` = reverse just the selected slice (the button lights up; reversed
    regions get tick marks along the bottom of the waveform), `DEL` = delete
    selected marker, `CLR` = wipe all, `>KIT` = spread chops onto a drumkit,
    `>PHR` = spread slices across the current phrase, `SHUF` = shuffle the
    phrase's steps (same hits, new order — the break dice; UNDO walks it back).
    `>KIT` is **zero-copy**: all pads reference slices of the original sample,
    so it works even when all 64 SampleBank slots are occupied.
  - waveform gestures: **tap** a marker or region = select + audition that
    slice; **drag from a marker** = move it. To delete, select a slice and press
    the explicit `DEL` button — double-tap deletion was removed because it is
    unreliable on the 3DS resistive touchscreen. The first cut at frame zero is
    locked; after deletion selection advances right (or left at the end).
    Marker movement snaps to zero crossings automatically (no clicks).
  - `>PHR` is the one-button break workflow: the instrument flips to
    chromatic-slice mode and each slice lands on the step nearest its real
    position in time (quantized to the 16-step grid), so uneven transient
    chops keep their groove. press play and the break plays like the source —
    then shuffle steps, reverse a slice, add `RET`, transpose in the chain.
  - re-pressing `>KIT` on an existing `chop NN` kit **rebuilds** its pads from
    the current markers (move a marker, press again, pads follow).
  - note: `TRNS`/`EQ`/`CLR` reset per-slice reverse flags, since re-chopping
    renumbers the slices.
- **LOAD** — WAV browser over `sdmc:/3ds/descry/wav/` (subfolders work).
  up/down select, A opens a folder / loads a file. 8/16/24/32-bit PCM and
  float WAVs are resampled to 32 kHz on load.
- **REC** — resample: one big arm/stop button. arming starts recording the
  **master output** (up to 15 s) and starts the song if stopped; stopping
  writes the recording into the instrument's sample slot. the tab glows while
  hot.

### drumkit

16 pads → 16 sample slots. top rows `TYPE / BASE / TABLE`, then a 4×4 pad
grid (A/B assign a sample slot per pad, down past the grid is `POLY`). `BASE`
is the midi note of pad 0 — notes in a phrase pick pads chromatically from
there.

bottom tabs: **KB / GEN**. the GEN panel is a 4×3 grid of procedural
generators — tap one and it synthesizes straight into the pad under the
cursor (allocating a free sample slot if the pad is empty):

`KICK · SNARE · CLHAT · OPHAT · CLAP · TOM · RIM · COWBELL · 808 · LEAD ·
PLUCK · PAD`

a full kit with zero files on the card.

### dsn voice

2 VCO / VCF / 2 EG / 2 MG analog-style voice (korg dsn-12 heritage). 37 params
in three columns:

| voice | envelopes | mod + amp |
| --- | --- | --- |
| TYPE, PRESET, OCT | EG1 A/D/S/R | MG1 wave, rate |
| OSC1 wave, PW | E1>PIT, E1>CUT | M1>PIT, M1>CUT |
| OSC2 wave, SEMI, DETUN | EG2 A/D/S/R | MG2 wave, rate |
| SYNC, BAL | E2>PIT, E2>CUT | M2>PW, M2>VCA |
| CUT, RES, FILT | | PORTA, VCA, VCALVL, DRIVE |

osc waves: saw/pulse/tri/sine/noise (5 options, wraps). `SYNC` hard-syncs
vco2 to vco1; `SEMI` ±24, `DETUN` ±50 cents; `BAL` mixes vco1↔vco2. both EGs
are full ADSR with signed routes to pitch and cutoff; both MGs (LFOs) have
4 waves and signed depths. `PORTA` = glide, `VCA` toggles gate/EG mode,
`DRIVE` = soft saturation.

presets: `INIT · ACID · HOOVER · SYNC · SUB · PWM · STRNGS · LEAD · PLUCK ·
WOBBLE · TREM · ZAP · WIND · KICK · SNARE · HAT · TOM`. `INIT` = single saw,
filter open, no modulation. the last four are analog drum
starting points — pitch-drop sine kick, tri+noise BPF snare, HPF noise hat,
sine tom. tweak decay/pitch-drop to taste.

---

## 5. FX commands

each phrase step has 3 FX slots: a command letter + a hex value. press
**SELECT on an FX command column** to open the built-in two-column reference —
tap a row to select, tap again (or A) to write it into the slot.

all timing is in **ticks**: a step = 6 ticks by default (groove changes this).

| cmd | name | value | what it does |
| --- | --- | --- | --- |
| VOL | volume | 00..FF | note volume |
| PIT | pitch | signed, 80=0 | pitch shift in semitones (81=+1, 7F=−1) |
| KIL | kill | 00..06 | hard-cut the note after xx ticks |
| RTG | retrigger | 00..06 | retrigger the note every xx ticks within the step |
| OFF | note-off | 00..06 | release: 00=now, xx=after xx ticks |
| ARP | arpeggio | xy nibbles | cycle base, +x, +y semitones across the step |
| DLY | note delay | 00..05 | defer the note-on by xx ticks (flam/ghost) |
| CUT | filter cutoff | 00..FF | 00=closed, FF=open |
| CRU | bitcrush | 00..FF | 00=clean (16 bit), FF=most crush |
| SND | send both | 00..FF | send level to delay + reverb |
| DEL | send delay | 00..FF | send to delay only |
| REV | send reverb | 00..FF | send to reverb only |
| RES | resonance | 00..FF | 00=clean, FF=self-oscillation |
| FTY | filter type | 00..04 | 0=LPF 1=HPF 2=BPF 3=notch 4=off |
| CHA | chance | 00..FF | trigger probability (80=50%, FF=always) |
| EVN | every | xy | play on pass x of every y phrase loops (14 = 1st of 4) |
| PAN | pan | signed, 80=center | 00=hard left, FF=hard right |
| LFO | lfo rate | 00..FF | per-track MG rate (~0.1 Hz..~20 Hz) |
| M>C | lfo→cutoff | signed, 80=0 | MG depth into filter cutoff |
| M>V | lfo→volume | signed, 80=0 | MG depth into amplitude |
| MGW | lfo wave | 00..03 | 0=TRI 1=SAW 2=SQR 3=S&H |
| BPM | tempo | 00..FF | set BPM from a step |
| HOP | hop | 00..0F | jump to step xx in this phrase |

enum-valued commands clamp at their last option — the value never scrolls into
a meaningless range.

### mod tables

a table (view: **table**) is 3 FX lanes × 16 rows that loop while the
instrument's note is alive — vibrato, filter sweeps, tremolo, auto-panning.
assign a table via the instrument's `TABLE` row (or SELECT in the table view).
**R+A / R+B** set the table's tick rate (`SPD 1..16` ticks per row). table
lanes accept the track-level subset that makes musical sense at tick rate:
`VOL PAN CUT RES CRU SND DEL REV LFO M>C M>V`.

### instrument FX defaults

every instrument carries a defaults strip (bottom of the instrument view):
`FLT · CUT · RES · DEL · REV · VOL · PAN · CRS`. these are applied whenever
the instrument triggers, before any step FX. press **ZL+SELECT** in the
instrument view to focus the strip; d-pad moves, A/B/X/Y edit, ZL+SELECT
again to leave.

---

## 6. performance

### kaoss pad

tap the **KEYS/PADS/KAOSS** button on the touchscreen to cycle the bottom band
into the XY pad. the left column has:

- **X** and **Y** assign buttons — tap to open a popup and pick a destination:
  `CUT · RES · DEL · REV · BIT · DSM · RAT · M>C · M>V · VOL · PAN`
- **TRK / ALL** — apply to the track under the song cursor, or all 8 at once

drag on the field to perform. a baseline is captured at gesture start; on
release everything **ramps back** to it — no stuck closed filters. the readout
shows target + both axis values live.

while a gesture is held, the pad **owns** its parameters — note triggers won't
stomp them. `CUT`, `RES` and `M>C` auto-engage a lowpass on instruments that
run filterless, and `M>C` pulls the base cutoff to ~55% so the wobble has
headroom to swing both ways.

### analog controls

analog input is an editing accelerator, not a second performance layer:

- **circle pad** — velocity-sensitive movement in the 256-row Song view. a light
  deflection steps slowly; a full deflection rapidly scans the arrangement. the
  d-pad remains the precise one-cell control on every screen.
- **C-stick up/down** — relative value encoder for the cell/parameter under the
  cursor. vertical motion edits by ±1; lean the C-stick sideways while moving it
  vertically for a coarse ±16 step.
- **sampler WAVE panel** — circle X scrubs the START/LENGTH window and circle Y
  zooms it (up = in, down = out).

analog input is suppressed while L/R/ZL modifiers are held and is inert in the
project manager, so drift cannot trigger destructive or file actions.

### live mode

in the song view: **Y** queues the chain under the cursor on its track — it
launches on the next 16-step bar (immediately if nothing is playing). **X**
queues a stop. the same press again cancels. the bottom-screen pads are
per-track **solo** toggles. this is the "launch clips" workflow: build columns
of chains and fire them per track.

### groove & swing

- **R+up/down** — global groove, ticks per step (1..24; 6 = straight 16ths).
- **R+left/right** — swing 0..50% (off-beat step offset).
- **groove pattern** — the `GRV` column in the mixer view: a 16-slot
  ticks-per-step ladder (A/B ±1, X = straight 6, Y = clear slot; the pattern
  ends at the first empty slot and loops). classic swing = `7,5`, hard shuffle
  = `8,4`. when the ladder is empty the global groove value drives everything.

### scale snap

tap the **KEY readout** (top bar of the touchscreen) to change the key: the
left half of the readout cycles the root note, the right half cycles the
scale (ZL+tap anywhere also cycles the root). 11 scales + off:

`MAJ · MIN · HRM · MEL · DOR · PHR · LYD · MIX · PMA · PMI · BLU`

with a key set, the touch keyboard snaps and highlights in-scale notes, the
pads become scale degrees, and A/B note editing walks in-scale. stored notes
are never rewritten — it's an input filter, not a quantizer.

### recording

the **REC** button on the touchscreen cycles the touch-keyboard mode:

- **JAM** (default) — keys and pads only preview. nothing is written. noodle
  freely over a playing song without leaving fingerprints.
- **WRT** — tracker write mode: in the phrase view, every key you tap lands at
  the cursor and the cursor advances one step. the fastest way to key in a line.
- **LIVE** — with the transport running, notes you play are recorded onto the
  step that is playing *right now* in the current phrase.

the **CLR** button next to it erases: in WRT it clears the step under the
cursor and advances (tap-tap-tap wipes a run), in LIVE it clears the currently
playing step. both are undoable (ZL+B).

---

## 7. sampling

- **mic record** — hold **ZR** anywhere: the built-in mic records into the
  current sample slot for as long as you hold it. release to stop. instant
  field-recording into the sampler.
- **resample** — the sampler's **REC** tab records the master output into the
  instrument's slot (up to 15 s, stereo). arm, perform (kaoss included), stop:
  the mix is now an instrument. see 4/sampler.
- **sample editor** — the sampler's **WAVE** tab: normalize, reverse, fade
  in/out, gain ±3 dB, crop, copy to another slot, root note, start/end markers,
  or turn the window into a persistent wavetable (`X` / `>WT`). **SLICE** tab:
  transient/equal auto-chop, tap-to-audition, chop→kit, slices→phrase.
- recorded/loaded samples persist as `sample_XX.s16` on the SD card.

---

## 8. files on SD

everything lives under `sdmc:/3ds/descry/`:

| path | what |
| --- | --- |
| `session.tr3d` | autosaved session (written on exit) |
| `project_XX.tr3d` | 16 save slots |
| `sample_XX.s16` | recorded/loaded samples |
| `wav/` | your WAVs for the sampler LOAD browser (subfolders work) |
| `wavetable/` | single-cycle WAVs for the wavsynth USER shape |
| `screens/` | screenshots (R+SELECT) |
| `renders/` | song exports — press **SELECT** in the project view (32 kHz stereo, streamed straight to the card, 10 min cap). the render carries your full mix (faders, mutes, sends, master) and a ~3 s tail for the delay/reverb. the file is named after the project (hold **R** in the project view to rename), and a `_01`, `_02`… suffix is added instead of overwriting an earlier take |

---

## 9. tips & gotchas

- **everything is hex.** velocity `7F` = max, chain `0A` = ten. embrace it.
- **track numbering starts at 0** — the readouts say T0..T7.
- **instruments are shared.** one more time: `I=00` in twenty phrases is *one*
  instrument. clone before you tweak (L+A in the instrument view).
- chains and phrases are shared the same way — ZL+SELECT clones (deep clone
  in the song view, phrase clone in the chain view).
- **R+Y** clears a whole phrase; **R+B** clears a step; **R+A** clears just
  the cell under the cursor.
- **ZL+B / ZL+A** = undo / redo, everywhere in the sequencer. in the instrument
  view they walk the *instrument* history instead, so a preset load or a type
  switch — which replaces every parameter and the name at once — is one press away
  from coming back.
- lost in FX letters? **SELECT on an FX column** opens the built-in command
  list with full names.
- hold **ZL, L or R** and look at the bottom-screen hint bar — it shows that
  modifier's live combo map for the current screen.
- **L+SELECT** = fullscreen oscilloscope. **R+SELECT** = screenshot of both
  screens to `screens/`.
- **the wet knobs need a send.** delay/reverb in the mixer do nothing until
  some track sends into the bus (instrument FX defaults, `SND` command, or
  kaoss DEL/REV). see 3/mixer signal flow.
- default note entry uses the current instrument slot — park the instrument
  view on the slot you're writing with and the `I` column fills itself.
- the header shows battery and clock; the project screen shows a dirty marker
  when you have unsaved changes. descry autosaves the session on clean exit,
  but save to a slot (project view, Y) before long jams anyway.
