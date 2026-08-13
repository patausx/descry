# ENGINE NOTES — контракты движка для генераторов треков

Шпаргалка, чтобы не перечитывать движок каждый раз. Всё проверено по коду (2026-08).

## тайминг

```
frames_per_tick = SR * 60 / (bpm * 4 * TICKS_PER_STEP)
TICKS_PER_STEP = 6     // m8-дефолт, step = 16-я нота
SR = 32000
```
- `song.groove` = ticks/step (дефолт 6). `song.groove_steps[16]` перекрывает: ненулевые слоты циклятся, 0 = конец паттерна. Классический свинг `{7,5}`, хард-шаффл `{8,4}`.
- `song.swing` 0..50 — % сдвига чётных степов.
- **вся FX-таймировка в ТИКАХ**, не в фреймах: KIL/OFF/DLY/RTG.
- ADSR инструментов — во ФРЕЙМАХ (32000 = 1 сек).

## иерархия и главная грабля

```
song.rows[256].chain[8]   -> chain id или EMPTY(0xFF)
chain.rows[16]            -> {phrase, transpose}
phrase.steps[16]          -> {note, instrument, velocity, fx[3]}
phrase.length 1..16       -> полиметрия (12 против 16 = дрейф акцентов)
```

**КАЖДЫЙ ТРЕК ИДЁТ ПО SONG-СЕТКЕ САМОСТОЯТЕЛЬНО.**
`next_chain_row()`: чейн кончился (row >= 16 ИЛИ следующая фраза EMPTY) → `next_song_row()` для ЭТОГО трека.

Отсюда железное правило:

> в одной song-строке все чейны ОБЯЗАНЫ быть одинаковой длины в фразах,
> иначе треки уезжают на следующую song-строку в разное время и аранжировка разъезжается.

Пустая ячейка (`EMPTY`) = один молчаливый 16-шаговый ряд, сетка сохраняется — так что дырки безопасны, а чейн из 1 фразы рядом с чейном из 4 — нет.

`song_content_rows()` = последняя строка с любым чейном + 1; на ней песня зацикливается и взводит `song_wrapped_`.

## FX (буква + байт)

| код | имя | значение |
|---|---|---|
| `V` | VOL | 00..FF |
| `P` | PIT | **signed: 80 = 0**, 81 = +1 полутон, 7F = −1 |
| `K` | KIL | хард-кат через xx **тиков** (max 6) |
| `X` | OFF | note-off через xx тиков (max 6) |
| `R` | RTG | ретриг, период в тиках (max 6) |
| `J` | ARP | ниблы: base, +x, +y полутонов |
| `D` | DLY | задержка note_on на xx тиков (max 5) — флэм/гост |
| `F` | CUT | катофф 00=закрыт FF=открыт |
| `Q` | RES | резонанс |
| `Y` | FTY | 0=LPF 1=HPF 2=BPF 3=Notch 4=Off |
| `B` | CRU | биткрашер |
| `S` | SND | send в delay+reverb |
| `E` | DEL | только delay |
| `G` | REV | только reverb |
| `O` | CHA | вероятность: 80 ≈ 50%, FF = всегда |
| `C` | EVN | **1-based** `xy` = играть на проходе x из каждых y (14 = 1-й из 4) |
| `A` | PAN | signed: 00=L, 80=центр, FF=R |
| `L`/`M`/`N`/`W` | MG rate / →cutoff / →vca / wave | signed для M,N (80=0) |
| `T` | TMP | bpm |
| `H` | HOP | прыжок на степ 0..F в этой фразе |

`fx_value_max()` режет enum-команды — значения выше просто недостижимы в UI, в файл писать их бессмысленно.

## инструменты

`InstrumentType`: 0 None, 1 Wavsynth, 2 Sampler, 3 DrumKit, 4 FmSynth, 5 DsnSynth.

Per-instrument FX-дефолты (применяются ДО степовых FX): `fx_filter_type` (0=off,1=LP,2=HP,3=BP,4=Notch), `fx_cutoff` 255=открыт, `fx_resonance`, `fx_send_del`, `fx_send_rev`, `fx_volume` 255=unity, `fx_pan` signed, `fx_bits` 16=чисто.

Пресеты одной строкой: `fm_load_preset(I.fm, FmPreset::Bass)`, `dsn_load_preset(I.dsn, DsnPreset::Acid)` (есть Kick/Snare/Hat/Tom!).

## сэмплы — САМОЕ ВАЖНОЕ

**`.tr3d` НЕ СОДЕРЖИТ АУДИО.** `SamplerParams.sample_slot` — просто индекс в глобальный `SampleBank` (64 слота), который наполняется отдельными файлами:

```
sdmc:/3ds/descry/sample_XX.s16     // XX = 00..63, десятичное
sdmc:/3ds/descry/sample_XX.name    // опционально, plain text
```

Формат `.s16` (v3):
```
uint32 magic 'TR3S' = 0x53335254
uint8  version = 3
uint8  channels (1|2)
uint8  root_note
uint8  flags (bit0 = reversed)
uint32 loop_start, loop_end        // = 16 байт заголовка
uint32 chops[32]                   // 0xFFFFFFFF = пусто, ФРЕЙМЫ
uint32 slice_rev_mask              // бит k = k-й слайс (в СОРТИРОВАННОМ порядке) задом наперёд
int16  pcm[]                       // мono: [frame], stereo: [frame*2+ch]
```
Загружаются на старте в оба слота независимо от проекта → демо-трек с сэмплами обязан ехать вместе с `.s16`.

### слайсы
- адресация в **сортированном** порядке маркеров, не по индексу массива.
- `chromatic_slices = true`: нота от `root_note` выбирает слайс, транспонирования НЕТ (semis=0).
- `PlayMode::Thru` — слайс стартует с маркера, но играет до конца сэмпла (райд звенит поверх брейка).
- `slice_rev_mask` — реверс отдельных слайсов, живёт в файле сэмпла. Реверс-снейр = джангл-классика.
- `DrumKitParams::set_sliced_sample(slot)` — zero-copy брейк-кит: пад N = слайс N.
- анти-клик фейд применяется к `play_end_` слайса, так что резать безопасно, щелчков не будет.

### beat sync (repitch)
`sync_bars = N` + `bar_frames = bar_frames_at_bpm(bpm)` → сэмпл длиной N баров подгоняется под темп проекта изменением скорости (амига/акай-стайл, с изменением высоты). Клампится 1/8x..8x.

## файлы проекта

```
sdmc:/3ds/descry/project_XX.tr3d   // XX = 00..0F, ШЕСТНАДЦАТЕРИЧНОЕ (!)
sdmc:/3ds/descry/session.tr3d      // автосейв
```
Заголовок: `magic 'TR3D' 0x44335254`, `version 13`, `project_size = sizeof(Project)`, `reserved`. Дальше — сырой `Project`.
Читаются версии 10..13 (старые как префикс + занулённый хвост). Добавил поле в `Instrument::union` → бампай `PROJECT_VERSION`.

## лимиты
256 chains, 256 phrases, 32 tables, 128 instruments, 8 треков × 4 голоса, 64 слота сэмплов, бюджет PCM 32 MiB.

## host-сборка генератора
```
g++ -std=c++17 -O2 -I. tools/gen_X.cpp \
  core/sequencer/project.cpp core/sequencer/serialize.cpp \
  core/synth/{wavsynth,sampler,drumkit,fm,dsn_synth,wavetable,wav_loader,granular}.cpp \
  core/audio/fixed.cpp -o /tmp/genX
```
Рендер и анализ на щелчки/клип: `tools/host_render.cpp <file.tr3d> <seconds>` → пишет `/tmp/render.wav`, печатает peak/maxjump/knee% по окнам 0.5 с.
