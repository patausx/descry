#include "player.h"
#include "project.h"
#include "fx.h"
#include <cstring>

namespace trackr::seq {

// shared per-track mixer DSP reset (used by play_song/play_phrase/play_chain
// and by the live-mode queued launches).
void Player::reset_track_fx(int track) {
    auto& tr = mixer_.track(track);
    tr.cutoff     = fx::Q15_ONE;
    tr.resonance  = 0;
    tr.filter_type = ::trackr::dsp::FilterType::LPF;
    tr.mg_to_cutoff = 0;
    tr.mg_to_vca    = 0;
    tr.mg_wave      = 0;
    tr.mg.reset_phase();
    tr.bits       = 16;
    tr.downsample = 1;
    tr.send_del   = 0;
    tr.send_rev   = 0;
    tr.volume     = fx::Q15_ONE;
    tr.pan        = 0;
    tr.perf_hold  = 0;   // release any kaoss/stick param ownership
}

void Player::play_song(uint16_t from_row) {
    audio::Mixer::LockGuard _g(mixer_);
    // reset FX state of all tracks to defaults (so the start is clean)
    for (int t = 0; t < NUM_TRACKS; ++t) {
        reset_track_fx(t);
        mixer_.track(t).muted = false;
    }
    for (int t = 0; t < NUM_TRACKS; ++t) {
        auto& ts = tracks_[t];
        ts.playing    = true;
        ts.song_mode_ = true;       // song drives by rows
        ts.song_row   = from_row;
        ts.chain_id   = project_.song.rows[from_row].chain[t];
        ts.chain_row  = 0;
        ts.phrase_id  = (ts.chain_id != EMPTY)
                      ? project_.chains[ts.chain_id].rows[0].phrase
                      : EMPTY;
        ts.transpose  = (ts.chain_id != EMPTY)
                      ? project_.chains[ts.chain_id].rows[0].transpose
                      : 0;
        ts.step = 0;
        ts.phrase_pass = 0;
    }
    any_playing_ = true;
    frames_to_next_tick_ = 0;
    tick_counter_ = 0;
    tick_in_step_ = 0;
    first_tick_ = true;
    swing_step_ = 0;
    groove_pos_ = 0;
    cur_tps_ = TICKS_PER_STEP;
}

void Player::play_phrase(int track, uint8_t phrase_id) {
    audio::Mixer::LockGuard _g(mixer_);
    auto& ts = tracks_[track];
    // reset the track's FX state so the phrase doesn't play through cutoff/send/bits left over from the song
    reset_track_fx(track);
    ts.playing    = true;
    ts.song_mode_ = false;          // one-shot call, don't loop through the song
    ts.song_row   = 0;
    ts.chain_id   = EMPTY;
    ts.chain_row  = 0;
    ts.phrase_id  = phrase_id;
    ts.transpose  = 0;
    ts.step = 0;
    ts.phrase_pass = 0;
    any_playing_ = true;
    frames_to_next_tick_ = 0;
    tick_counter_ = 0;
    tick_in_step_ = 0;
    first_tick_ = true;
    swing_step_ = 0;
    groove_pos_ = 0;
    cur_tps_ = TICKS_PER_STEP;
}

void Player::play_chain(int track, uint8_t chain_id) {
    audio::Mixer::LockGuard _g(mixer_);
    if (chain_id == EMPTY || project_.chains[chain_id].rows[0].phrase == EMPTY) return;
    auto& ts = tracks_[track];
    // reset the track's FX state (same as play_phrase)
    reset_track_fx(track);
    ts.playing    = true;
    ts.song_mode_ = false;          // chain loops on itself (m8 chain-preview mode)
    ts.song_row   = 0;
    ts.chain_id   = chain_id;
    ts.chain_row  = 0;
    ts.phrase_id  = project_.chains[chain_id].rows[0].phrase;
    ts.transpose  = project_.chains[chain_id].rows[0].transpose;
    ts.step = 0;
    ts.phrase_pass = 0;
    any_playing_ = true;
    frames_to_next_tick_ = 0;
    tick_counter_ = 0;
    tick_in_step_ = 0;
    first_tick_ = true;
    swing_step_ = 0;
    groove_pos_ = 0;
    cur_tps_ = TICKS_PER_STEP;
}

void Player::stop() {
    audio::Mixer::LockGuard _g(mixer_);
    for (int t = 0; t < NUM_TRACKS; ++t) {
        tracks_[t].playing = false;
        tracks_[t].table_active = false;
        // remove all voices of the track (STOP - immediately)
        mixer_.clear_voices(t);
        // reset FX state
        auto& tr = mixer_.track(t);
        tr.cutoff     = fx::Q15_ONE;
        tr.resonance  = 0;
        tr.filter_type = ::trackr::dsp::FilterType::LPF;
        tr.mg_to_cutoff = 0;
        tr.mg_to_vca    = 0;
        tr.mg_wave      = 0;
        tr.mg.reset_phase();
        tr.bits       = 16;
        tr.downsample = 1;
        tr.send_del   = 0;
        tr.send_rev   = 0;
        tr.volume     = fx::Q15_ONE;
        tr.pan        = 0;
        tr.perf_hold  = 0;
    }
    // clear reverb and delay tails so everything goes silent + DC blocker state
    mixer_.reverb.reset();
    std::memset(mixer_.delay_l, 0, sizeof(mixer_.delay_l));
    std::memset(mixer_.delay_r, 0, sizeof(mixer_.delay_r));
    mixer_.reset_master_state();
    any_playing_ = false;
}

void Player::stop_track(int track) {
    audio::Mixer::LockGuard _g(mixer_);
    tracks_[track].playing = false;
    mixer_.note_off_all(track);
}

void Player::apply_track_fx(int track, uint8_t cmd, uint8_t val) {
    switch (cmd) {
        case fx_cmd::VOL: {
            int v = (int)val * fx::Q15_ONE / 255;
            mixer_.track(track).volume = (fx::q15)v;
            break;
        }
        case fx_cmd::PAN: {
            // signed pan: 00=hard L, 80=center, FF=hard R. pan>0 cuts L (=right).
            int signed_val = (int)val - 0x80;             // -128..+127
            int p = signed_val * fx::Q15_ONE / 128;       // -ONE..+ONE
            if (p < -fx::Q15_ONE) p = -fx::Q15_ONE;
            if (p >  fx::Q15_ONE) p =  fx::Q15_ONE;
            mixer_.track(track).pan = (fx::q15)p;
            break;
        }
        case fx_cmd::FIL: {
            int v = (int)val * fx::Q15_ONE / 255;
            auto& tr = mixer_.track(track);
            // auto-enable LPF if the filter is Off - otherwise CUT is silent and the
            // user thinks the command is broken. (M8-style: CUT just works.)
            if (tr.filter_type == ::trackr::dsp::FilterType::Off)
                tr.filter_type = ::trackr::dsp::FilterType::LPF;
            tr.cutoff = (fx::q15)v;
            break;
        }
        case fx_cmd::CRU: {
            // bitcrush: bite earlier. val 00=clean(16b), FF=1bit. curve: bits drop fast.
            int b = 16 - ((int)val * 16 / 255);
            if (b < 1) b = 1;
            if (b > 16) b = 16;
            mixer_.track(track).bits = (uint8_t)b;
            // sample-rate decimation kicks in on the upper half for a grittier sound
            if (val > 128) {
                int ds = 2 + ((val - 128) * 10 / 127);
                mixer_.track(track).downsample = (uint8_t)ds;
            } else {
                mixer_.track(track).downsample = 1;
            }
            break;
        }
        case fx_cmd::SND: {
            // send to BOTH delay+reverb buses. boosted ~1.5x to overcome the -12dB bus
            // headroom, so even mid values are clearly audible. clamped to ONE.
            int v = (int)val * fx::Q15_ONE / 255;
            v = v + (v >> 1);                  // x1.5
            if (v > fx::Q15_ONE) v = fx::Q15_ONE;
            mixer_.track(track).send_del = (fx::q15)v;
            mixer_.track(track).send_rev = (fx::q15)v;
            break;
        }
        case fx_cmd::SDL: {
            // send to delay only (m8 DEL)
            int v = (int)val * fx::Q15_ONE / 255;
            v = v + (v >> 1);
            if (v > fx::Q15_ONE) v = fx::Q15_ONE;
            mixer_.track(track).send_del = (fx::q15)v;
            break;
        }
        case fx_cmd::SRV: {
            // send to reverb only (m8 REV)
            int v = (int)val * fx::Q15_ONE / 255;
            v = v + (v >> 1);
            if (v > fx::Q15_ONE) v = fx::Q15_ONE;
            mixer_.track(track).send_rev = (fx::q15)v;
            break;
        }
        case fx_cmd::RES: {
            // resonance: quadratic curve so low values are subtle and the top end
            // pushes hard into self-oscillation (linear felt 'only audible at max').
            int lin = (int)val * fx::Q15_ONE / 255;
            int v = ((int)((int64_t)lin * lin / fx::Q15_ONE));   // v^2 normalized
            // bias up a touch so mid values already sing
            v = lin / 4 + v * 3 / 4;
            if (v > fx::Q15_ONE) v = fx::Q15_ONE;
            auto& tr = mixer_.track(track);
            // resonance is only audible with an active filter - auto-enable LPF if Off.
            if (tr.filter_type == ::trackr::dsp::FilterType::Off)
                tr.filter_type = ::trackr::dsp::FilterType::LPF;
            tr.resonance = (fx::q15)v;
            break;
        }
        case fx_cmd::FTY: {
            // filter type selection: 0=LPF, 1=HPF, 2=BPF, 3=Notch, 4=Off
            ::trackr::dsp::FilterType ft;
            switch (val) {
                case 1:  ft = ::trackr::dsp::FilterType::HPF; break;
                case 2:  ft = ::trackr::dsp::FilterType::BPF; break;
                case 3:  ft = ::trackr::dsp::FilterType::Notch; break;
                case 4:  ft = ::trackr::dsp::FilterType::Off; break;
                default: ft = ::trackr::dsp::FilterType::LPF; break;
            }
            auto& tr = mixer_.track(track);
            tr.filter_type = ft;
            // HPF/BPF/Notch are inaudible with a wide-open cutoff. if the user just picks
            // a type without touching CUT, pull cutoff into a musical mid-zone so the
            // effect is immediately obvious. (LPF stays open = no surprise gating.)
            if (ft != ::trackr::dsp::FilterType::LPF &&
                ft != ::trackr::dsp::FilterType::Off &&
                tr.cutoff > fx::Q15_ONE - 64) {
                tr.cutoff = fx::Q15_ONE / 2;   // ~half-open
            }
            break;
        }
        case fx_cmd::LFR: {
            // MG rate: 00=slow, FF=fast
            int v = (int)val * fx::Q15_ONE / 255;
            mixer_.track(track).mg_rate = (fx::q15)v;
            break;
        }
        case fx_cmd::MGC: {
            // MG → cutoff depth, signed: 80=0, FF=+max, 00=-max
            int signed_val = (int)val - 0x80;       // -128..+127
            int depth = signed_val * fx::Q15_ONE / 128;  // -ONE..+ONE
            mixer_.track(track).mg_to_cutoff = (int16_t)depth;
            break;
        }
        case fx_cmd::MGV: {
            // MG → VCA depth, signed
            int signed_val = (int)val - 0x80;
            int depth = signed_val * fx::Q15_ONE / 128;
            mixer_.track(track).mg_to_vca = (int16_t)depth;
            break;
        }
        case fx_cmd::MGW: {
            // MG waveform: 0=TRI 1=SAW 2=SQR 3=S&H
            mixer_.track(track).mg_wave = (uint8_t)(val & 3);
            break;
        }
        case fx_cmd::TMP: {
            int t = val;
            if (t < 30) t = 30;
            if (t > 255) t = 255;
            project_.song.bpm = (uint8_t)t;
            break;
        }
    }
}

void Player::apply_table_tick(int track) {
    auto& ts = tracks_[track];
    if (!ts.table_active || ts.table_id == EMPTY) return;
    if (ts.table_id >= MAX_TABLES) { ts.table_active = false; return; }

    // SPD divider: advance one row every `speed` ticks (m8 TICxx). 0 = legacy = 1.
    uint8_t spd = project_.table_speed[ts.table_id];
    if (spd < 1) spd = 1;
    if (++ts.table_tick_ctr < spd) return;
    ts.table_tick_ctr = 0;

    const auto& row = project_.tables[ts.table_id].rows[ts.table_row];
    for (int s = 0; s < 3; ++s) {
        if (row.fx[s].cmd != 0) {
            apply_track_fx(track, row.fx[s].cmd, row.fx[s].value);
        }
    }
    // advance the row in the loop
    ts.table_row = (ts.table_row + 1) % TABLE_ROWS;
}

void Player::fire_note(int track, int note, uint8_t inst, uint8_t vel) {
    if (inst == EMPTY) return;
    auto& ts = tracks_[track];
    // sidechain duck: a note on the source track pumps the global duck envelope
    if (track == (int)project_.song.duck_src)
        mixer_.trigger_duck();
    if (!project_.instruments[inst].poly) {
        mixer_.note_off_all(track);
    }
    auto* v = project_.make_voice(inst);
    if (v) {
        if (note < 0) note = 0;
        if (note > 127) note = 127;
        v->note_on(note, vel);
        mixer_.replace_voice(track, v);
    }
    (void)ts;
}

// shared: push the instrument's FX defaults into the mixer track state.
// used by trigger_step (sequencer) AND by UI previews (touch keyboard, SELECT),
// so what you hear while editing == what plays in the sequence.
void Player::apply_inst_fx_defaults(const Instrument& inst, audio::TrackState& mt) {
    // performance hold: parameters currently owned by a kaoss/stick gesture
    // are skipped - otherwise every note trigger stomps the live gesture
    // (this was why kaoss cutoff/res "did nothing" during playback).
    const uint16_t hold = mt.perf_hold;
    // filter type: 0=off->Off, 1=LP, 2=HP, 3=BP, 4=Notch
    if (!(hold & audio::TrackState::HOLD_FLT)) {
        switch (inst.fx_filter_type) {
            case 1: mt.filter_type = dsp::FilterType::LPF;   break;
            case 2: mt.filter_type = dsp::FilterType::HPF;   break;
            case 3: mt.filter_type = dsp::FilterType::BPF;   break;
            case 4: mt.filter_type = dsp::FilterType::Notch; break;
            default: mt.filter_type = dsp::FilterType::Off;  break;
        }
    }
    if (!(hold & audio::TrackState::HOLD_CUT))
        mt.cutoff    = (fx::q15)((int)inst.fx_cutoff    * fx::Q15_ONE / 255);
    if (!(hold & audio::TrackState::HOLD_RES))
        mt.resonance = (fx::q15)((int)inst.fx_resonance * fx::Q15_ONE / 255);
    if (!(hold & audio::TrackState::HOLD_DEL))
        mt.send_del  = (fx::q15)((int)inst.fx_send_del  * fx::Q15_ONE / 255);
    if (!(hold & audio::TrackState::HOLD_REV))
        mt.send_rev  = (fx::q15)((int)inst.fx_send_rev  * fx::Q15_ONE / 255);
    if (!(hold & audio::TrackState::HOLD_VOL))
        mt.volume    = (fx::q15)((int)inst.fx_volume    * fx::Q15_ONE / 255);
    if (!(hold & audio::TrackState::HOLD_PAN))
        mt.pan       = (fx::q15)((int)inst.fx_pan       * fx::Q15_ONE / 128);  // -ONE..+ONE
    if (!(hold & audio::TrackState::HOLD_BIT))
        mt.bits      = inst.fx_bits;
}

void Player::trigger_step(int track) {
    auto& ts = tracks_[track];
    if (ts.phrase_id == EMPTY) return;

    const auto& step = project_.phrases[ts.phrase_id].steps[ts.step];

    // === apply this instrument's FX defaults to the track BEFORE the step's own
    // FX commands (so FIL/SND/RES in the phrase override them). only on a real note. ===
    if (step.note != EMPTY && step.instrument != EMPTY &&
        step.instrument < MAX_INSTRUMENTS) {
        apply_inst_fx_defaults(project_.instruments[step.instrument], mixer_.track(track));
    }

    // reset intra-step FX state from the previous step
    ts.retrig_count = 0;
    ts.retrig_remaining = 0;
    ts.arp_period = 0;
    ts.arp_remaining = 0;
    ts.arp_len = 0;
    ts.delay_ticks = 0;
    ts.kill_ticks = 0;

    // check effects on all three fx slots BEFORE triggering the note
    // (this is needed for V - it sets the volume before the note)
    int8_t  fx_pitch = 0;
    bool    has_pitch = false;
    int     chance = -1;          // -1 = no O command (always play); 0..255 = probability
    int     evn_val = -1;         // -1 = no C command; else xy nibbles (pass x of y)
    int     arp_val = -1;         // -1 = no ARP; else xy nibbles
    int     dly_ticks = 0;        // 0 = no delay
    for (int s = 0; s < 3; ++s) {
        uint8_t cmd = step.fx[s].cmd;
        uint8_t val = step.fx[s].value;
        if (cmd == fx_cmd::PIT) {
            // signed pitch in semitones: 80=0, 81=+1, 7F=-1. clamped to +/-12 (one octave).
            int p = (int)val - 0x80;
            if (p >  12) p =  12;
            if (p < -12) p = -12;
            fx_pitch = (int8_t)p;
            has_pitch = true;
        } else if (cmd == fx_cmd::CHA) {
            chance = val;          // trigger chance: roll happens below
        } else if (cmd == fx_cmd::EVN) {
            evn_val = val;         // conditional trigger: checked below, next to chance
        } else if (cmd == fx_cmd::OFF) {
            // X xx - note-off (release). 00 = release now, N = release after N ticks.
            if (val == 0) mixer_.note_off_all(track);
            else { ts.kill_ticks = val; ts.kill_is_cut = false; }
        } else if (cmd == fx_cmd::ARP) {
            arp_val = val;         // applied after note_on
        } else if (cmd == fx_cmd::DLY) {
            // D xx - delay note_on by N ticks. clamp below one step so the deferred note
            // fires within its own step (the next step's reset would otherwise eat it).
            dly_ticks = val;
            if (dly_ticks >= TICKS_PER_STEP) dly_ticks = TICKS_PER_STEP - 1;
        } else {
            // common V/F/B/T moved into a helper
            apply_track_fx(track, cmd, val);
        }
    }

    // EVN condition (C xy): deterministic gate - play only on pass x of every y
    // phrase loops (1-based x). y=0/1 -> always. the counterpart to CHA's dice:
    // 'C 24' = only the 2nd of every 4 passes. checked BEFORE the chance roll so
    // combining C+O gives "every Nth pass, with probability".
    if (evn_val >= 0) {
        int x = (evn_val >> 4) & 0x0F;
        int y = evn_val & 0x0F;
        if (y > 1) {
            if (x < 1) x = 1;
            if (x > y) x = y;
            if ((int)(ts.phrase_pass % (uint8_t)y) != x - 1) return;
        }
    }

    // trigger chance (O command): roll the dice. FF=always, 00=never.
    if (chance >= 0) {
        uint32_t roll = rng_next() & 0xFF;   // 0..255
        if (roll >= (uint32_t)chance) return;  // failed the roll -> skip this step entirely
    }

    if (step.note != EMPTY && step.instrument != EMPTY) {
        int base_note = (int)step.note + ts.transpose + (has_pitch ? fx_pitch : 0);
        ts.last_note     = step.note;
        ts.last_inst     = step.instrument;
        ts.last_velocity = step.velocity;

        if (dly_ticks > 0) {
            // DLY: defer the note_on by N ticks.
            ts.delay_note  = (uint8_t)base_note;
            ts.delay_inst  = step.instrument;
            ts.delay_vel   = step.velocity;
            ts.delay_ticks = dly_ticks;
        } else {
            fire_note(track, base_note, step.instrument, step.velocity);
        }

        // ARP: cycle base, +hi-nibble, +lo-nibble semitones, one note per N ticks.
        if (arp_val >= 0) {
            ts.arp_offs[0] = 0;
            ts.arp_offs[1] = (uint8_t)((arp_val >> 4) & 0x0F);
            ts.arp_offs[2] = (uint8_t)(arp_val & 0x0F);
            ts.arp_len = (ts.arp_offs[2] != 0) ? 3 : (ts.arp_offs[1] != 0 ? 2 : 1);
            ts.arp_idx = 0;
            // one arp note per tick (classic M8 feel). at 6 ticks/step a 3-note arp
            // cycles twice per step.
            ts.arp_period = 1;
            ts.arp_remaining = ts.arp_period;
        }

        // start the mod table from the instrument
        uint8_t tid = project_.instruments[step.instrument].table_id;
        if (tid != EMPTY && tid < MAX_TABLES) {
            ts.table_id = tid;
            ts.table_row = 0;
            ts.table_tick_ctr = 0;
            ts.table_active = true;
        } else {
            ts.table_active = false;
            ts.table_id = EMPTY;
        }
    }

    // second pass for effects evaluated AFTER note_on (KIL/RET/HOP), in ticks.
    for (int s = 0; s < 3; ++s) {
        uint8_t cmd = step.fx[s].cmd;
        uint8_t val = step.fx[s].value;
        switch (cmd) {
            case fx_cmd::KIL: {
                // K xx - hard-cut the note after N ticks (00 = 1 tick = shortest gate).
                int n = (val == 0 ? 1 : val);
                ts.kill_ticks = n;
                ts.kill_is_cut = true;   // KIL = hard cut (vs OFF = soft release)
                break;
            }
            case fx_cmd::RTG: {
                // R xx - retrigger every xx ticks. value = tick PERIOD (M8-style), 01 = every
                // tick (densest). clamped to TICKS_PER_STEP so it always fires at least once
                // per step (a period longer than a step would produce no retriggers).
                int period = (val == 0 ? 1 : val);
                if (period > TICKS_PER_STEP) period = TICKS_PER_STEP;
                ts.retrig_period = period;
                ts.retrig_remaining = period;
                ts.retrig_count = 255;        // effectively 'until next step'
                break;
            }
            case fx_cmd::HOP: {
                // H xx - jump to step xx (mod 16). stored as a pending target:
                // on_tick applies it INSTEAD of the ++step, so xx itself plays next
                // (direct ts.step = xx was off by one - the increment ate it).
                ts.hop_target = (int8_t)(val & 0x0F);
                break;
            }
        }
    }
}

void Player::next_chain_row(int track) {
    auto& ts = tracks_[track];
    // EVN condition: one more pass of this track's phrase cycle completed.
    // counts every phrase restart (wrap or row advance), wraps naturally at 256.
    ++ts.phrase_pass;
    if (ts.chain_id == EMPTY) {
        // song mode: an empty cell = one silent 16-step row - keep following the
        // song grid so the track can (re)join when a later row has a chain.
        if (ts.song_mode_) { next_song_row(track); return; }
        // direct play_phrase without a chain - loop the phrase (preview) until the user presses STOP
        ts.step = 0;
        return;
    }
    ++ts.chain_row;
    if (ts.chain_row >= CHAIN_ROWS ||
        project_.chains[ts.chain_id].rows[ts.chain_row].phrase == EMPTY) {
        // chain ended - in song mode go to the next row,
        // in chain-only mode loop from the start
        if (ts.song_mode_) {
            next_song_row(track);
        } else {
            // chain loop - return to row 0
            ts.chain_row = 0;
            ts.phrase_id = project_.chains[ts.chain_id].rows[0].phrase;
            ts.transpose = project_.chains[ts.chain_id].rows[0].transpose;
            ts.step = 0;
        }
        return;
    }
    ts.phrase_id = project_.chains[ts.chain_id].rows[ts.chain_row].phrase;
    ts.transpose = project_.chains[ts.chain_id].rows[ts.chain_row].transpose;
    ts.step = 0;
}

// last song row that has ANY chain on any track, +1. the song loops there
// instead of marching through 256 mostly-empty rows.
int Player::song_content_rows() const {
    for (int r = SONG_ROWS - 1; r >= 0; --r)
        for (int t = 0; t < NUM_TRACKS; ++t)
            if (project_.song.rows[r].chain[t] != EMPTY) return r + 1;
    return 0;
}

void Player::next_song_row(int track) {
    auto& ts = tracks_[track];
    ++ts.song_row;
    int len = song_content_rows();
    if (len == 0) { ts.playing = false; return; }   // empty song - nothing to follow
    if (ts.song_row >= len) ts.song_row = 0;         // loop at the end of content
    ts.chain_id = project_.song.rows[ts.song_row].chain[track];
    ts.chain_row = 0;
    ts.step = 0;
    if (ts.chain_id == EMPTY) {
        // silent row: stay playing, keep the grid - rejoin when a chain appears
        ts.phrase_id = EMPTY;
        ts.transpose = 0;
        return;
    }
    ts.phrase_id = project_.chains[ts.chain_id].rows[0].phrase;
    ts.transpose = project_.chains[ts.chain_id].rows[0].transpose;
}

// pull the ticks-per-step for the step that is starting now.
// groove pattern (song.groove_steps) takes priority: non-empty slots cycle;
// an all-empty pattern falls back to the global `groove` value.
int Player::groove_tps_next() {
    const auto& gs = project_.song.groove_steps;
    // count valid pattern length (leading run of non-zero slots)
    int len = 0;
    while (len < PHRASE_STEPS && gs[len] != 0) ++len;
    if (len == 0) {
        int tps = project_.song.groove;
        return tps < 1 ? 1 : tps;
    }
    int tps = gs[groove_pos_ % (uint32_t)len];
    ++groove_pos_;
    return tps < 1 ? 1 : tps;
}

// === live mode: quantized chain launch ===
// queue (or toggle off) a chain on a track. starts at the next 16-step bar
// boundary of the global clock. QUEUE_STOP = queue a track stop.
void Player::queue_chain(int track, uint8_t chain_id) {
    audio::Mixer::LockGuard _g(mixer_);
    if (track < 0 || track >= NUM_TRACKS) return;
    // validate a real chain (stops are always valid)
    if (chain_id != QUEUE_STOP &&
        (chain_id == EMPTY || project_.chains[chain_id].rows[0].phrase == EMPTY))
        return;

    // toggle: queueing the same thing again cancels it
    if (queued_chain_[track] == chain_id) {
        queued_chain_[track] = EMPTY;
        return;
    }

    // nothing playing anywhere: start immediately, this establishes the bar clock
    if (!any_playing_) {
        if (chain_id != QUEUE_STOP) {
            play_chain(track, chain_id);
            global_step_ = 0;
        }
        return;
    }
    queued_chain_[track] = chain_id;
}

// called on every global step boundary; fires queued launches when the bar wraps.
void Player::launch_queued() {
    if (global_step_ % LIVE_QUANT_STEPS != 0) return;
    for (int t = 0; t < NUM_TRACKS; ++t) {
        uint8_t q = queued_chain_[t];
        if (q == EMPTY) continue;
        queued_chain_[t] = EMPTY;
        auto& ts = tracks_[t];
        if (q == QUEUE_STOP) {
            mixer_.note_off_all(t);
            ts.playing = false;
            ts.table_active = false;
            continue;
        }
        // inline chain start (play_chain resets the tick clock - here the clock
        // keeps running, we just repoint the track: that's the whole trick)
        reset_track_fx(t);
        ts.playing    = true;
        ts.song_mode_ = false;
        ts.chain_id   = q;
        ts.chain_row  = 0;
        ts.phrase_id  = project_.chains[q].rows[0].phrase;
        ts.transpose  = project_.chains[q].rows[0].transpose;
        ts.step = 0;
        ts.phrase_pass = 0;
        ts.hop_target = -1;
        any_playing_ = true;
    }
}

// one tick has elapsed: run per-tick FX for every track, and on step boundaries
// trigger the next step. this is the heart of the M8-style sequencer clock.
void Player::on_tick() {
    // --- step boundary: every TICKS_PER_STEP ticks we move to the next step ---
    bool step_now = (tick_in_step_ == 0);

    // live mode: queued chains launch on the bar grid (before tracks trigger,
    // so a launched chain plays its step 0 on this very tick)
    if (step_now) launch_queued();

    bool still_playing = false;
    for (int t = 0; t < NUM_TRACKS; ++t) {
        auto& ts = tracks_[t];
        if (!ts.playing) continue;

        if (step_now) {
            trigger_step(t);
            // HOP: pending jump replaces the normal step advance so the target
            // step itself is the next one triggered.
            if (ts.hop_target >= 0) {
                ts.step = (uint8_t)ts.hop_target;
                ts.hop_target = -1;
            } else {
                ++ts.step;
                // wrap at the CURRENT phrase's playable length (1..16, polymetry).
                // no active phrase = full 16 so empty tracks stay in global sync.
                int plen = PHRASE_STEPS;
                if (ts.phrase_id != EMPTY)
                    plen = phrase_len(project_.phrases[ts.phrase_id]);
                if (ts.step >= plen) {
                    ts.step = 0;
                    next_chain_row(t);
                }
            }
        }

        // advance the mod table every TICK (m8 TIC01 behavior). per-step advance
        // made 16-row sweeps last a whole phrase - 6x slower than intended.
        if (ts.playing && ts.table_active) {
            if (mixer_.primary_voice(t)) apply_table_tick(t);
            else ts.table_active = false;
        }

        // --- per-tick FX (counted in ticks) ---

        // DLY: deferred note_on
        if (ts.delay_ticks > 0) {
            --ts.delay_ticks;
            if (ts.delay_ticks == 0)
                fire_note(t, (int)ts.delay_note, ts.delay_inst, ts.delay_vel);
        }

        // RET: retrigger every retrig_period ticks
        if (ts.retrig_count > 0 && ts.retrig_period > 0) {
            --ts.retrig_remaining;
            if (ts.retrig_remaining <= 0) {
                fire_note(t, (int)ts.last_note + ts.transpose, ts.last_inst, ts.last_velocity);
                ts.retrig_remaining = ts.retrig_period;
            }
        }

        // ARP: cycle to the next offset every arp_period ticks
        if (ts.arp_period > 0 && ts.arp_len > 0) {
            --ts.arp_remaining;
            if (ts.arp_remaining <= 0) {
                ts.arp_idx = (uint8_t)((ts.arp_idx + 1) % ts.arp_len);
                int note = (int)ts.last_note + ts.transpose + (int)ts.arp_offs[ts.arp_idx];
                fire_note(t, note, ts.last_inst, ts.last_velocity);
                ts.arp_remaining = ts.arp_period;
            }
        }

        // KIL/OFF: cut or release after N ticks
        if (ts.kill_ticks > 0) {
            --ts.kill_ticks;
            if (ts.kill_ticks == 0) {
                if (ts.kill_is_cut) mixer_.cut_all(t);        // KIL = hard cut
                else                mixer_.note_off_all(t);   // OFF = soft release
            }
        }

        if (ts.playing) still_playing = true;
    }
    any_playing_ = still_playing;

    // advance the step-tick counter. the current step lasts cur_tps_ ticks;
    // when a new step starts we pull the next value from the groove pattern
    // (or the global groove fallback).
    if (step_now) cur_tps_ = groove_tps_next();
    if (++tick_in_step_ >= cur_tps_) tick_in_step_ = 0;
    ++tick_counter_;
    if (step_now) ++global_step_;
}

void Player::advance(std::size_t frames, int sample_rate) {
    // legacy whole-window variant (host tools). same semantics as before:
    // all ticks in the window fire, frame accounting is kept.
    int32_t remaining = static_cast<int32_t>(frames);
    while (remaining > 0)
        remaining -= advance_upto(remaining, sample_rate);
}

// fire any due ticks at the current position, then consume up to max_frames
// of silence-between-ticks. returns the consumed count (always >= 1 when
// max_frames >= 1) so the audio worker can render exactly that many frames
// before asking again -> note-ons land sample-accurate inside the buffer
// instead of being quantized to buffer starts (was: up to 32ms of jitter).
int32_t Player::advance_upto(int32_t max_frames, int sample_rate) {
    audio::Mixer::LockGuard _g(mixer_);
    if (max_frames <= 0) return 0;
    if (!any_playing_) return max_frames;

    // length of ONE TICK in frames. a step lasts `groove` ticks (M8-style).
    // bpm counts quarter notes; *4 = sixteenths (= steps); *TICKS_PER_STEP = ticks.
    // the tick duration stays anchored to TICKS_PER_STEP=6 so BPM matches tap-tempo at
    // the default groove; groove then sets how many ticks each step holds.
    int frames_per_tick = (sample_rate * 60)
                        / (project_.song.bpm * 4 * TICKS_PER_STEP);
    if (frames_per_tick < 1) frames_per_tick = 1;

    // fire the very first tick immediately on play start
    if (first_tick_) {
        first_tick_ = false;
        frames_to_next_tick_ = 0;
    }

    while (frames_to_next_tick_ <= 0) {
        // remember whether this tick STARTS a new step (before on_tick advances counters)
        bool step_start = (tick_in_step_ == 0);
        on_tick();

        // === SWING / SHUFFLE ===
        // swing delays every other STEP: odd-numbered steps start later by stealing
        // time from the preceding even step. swing 0 = straight, swing 50 = max shuffle.
        // applied only on the tick that begins a step (so the whole step shifts).
        int swing = project_.song.swing;
        int dur = frames_per_tick;
        if (swing > 0 && step_start) {
            // step parity: even steps lengthen, the following odd steps shorten.
            // tick_counter_ already advanced; the step we just started is counted below.
            int offset = (frames_per_tick * swing) / 100;
            bool even_step = ((swing_step_ & 1) == 0);
            dur = even_step ? (frames_per_tick + offset) : (frames_per_tick - offset);
            if (dur < 1) dur = 1;
            ++swing_step_;
        }
        frames_to_next_tick_ = dur;
        if (!any_playing_) return max_frames;   // everything stopped on this tick
    }

    int32_t consume = (frames_to_next_tick_ < max_frames) ? frames_to_next_tick_
                                                          : max_frames;
    frames_to_next_tick_ -= consume;
    return consume;
}

} // namespace trackr::seq
