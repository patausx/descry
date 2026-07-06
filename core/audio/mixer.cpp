#include "mixer.h"
#include <cstring>
#include <cmath>

#ifdef __3DS__
  #include <3ds.h>
#endif

namespace trackr::audio {

// === thread-safety helpers ===
void Mixer::lock() {
#ifdef __3DS__
    if (audio_lock_) RecursiveLock_Lock(static_cast<RecursiveLock*>(audio_lock_));
#endif
}
void Mixer::unlock() {
#ifdef __3DS__
    if (audio_lock_) RecursiveLock_Unlock(static_cast<RecursiveLock*>(audio_lock_));
#endif
}

// === pool helpers ===

void Mixer::replace_voice(int i, Voice* new_voice) {
    LockGuard _g(*this);
    auto& t = tracks_[i];
    // 1) look for an empty slot
    int slot = -1;
    for (int s = 0; s < TRACK_POLY; ++s) {
        if (!t.voices[s]) { slot = s; break; }
    }
    // 2) if none - steal: prefer the QUIETEST released voice (steal_weight),
    //    tie-break by age. moonlight-class material (dense poly, long release
    //    tails) steals every note - grabbing a loud ringing voice was an
    //    audible tick each time; the quietest tail is barely missed.
    if (slot < 0) {
        uint32_t oldest_age = 0xFFFFFFFFu;
        fx::q31  best_w     = 0x7FFFFFFF;
        for (int s = 0; s < TRACK_POLY; ++s) {
            fx::q31 w = t.voices[s] ? t.voices[s]->steal_weight() : 0;
            if (w < best_w || (w == best_w && t.voice_age[s] < oldest_age)) {
                best_w = w;
                oldest_age = t.voice_age[s];
                slot = s;
            }
        }
        // anti-click: render the stolen voice's next 2ms with a linear fade
        // into the track's tail buffer (mixed in by the next render chunk),
        // THEN delete. a hard delete mid-wave was an audible click - constant
        // crackle on slow poly material (long releases = pool always full).
        Voice* old = t.voices[slot];
        if (old && old->active()) {
            fx::q15 tmp[TrackState::FADE_TAIL * 2];
            old->render(tmp, TrackState::FADE_TAIL);
            for (std::size_t f = 0; f < TrackState::FADE_TAIL; ++f) {
                // linear fade 1 -> 0 across the tail
                int32_t g = (int32_t)(TrackState::FADE_TAIL - f);
                t.fade_tail[f*2 + 0] += ((fx::q31)tmp[f*2 + 0] * g) / (int32_t)TrackState::FADE_TAIL;
                t.fade_tail[f*2 + 1] += ((fx::q31)tmp[f*2 + 1] * g) / (int32_t)TrackState::FADE_TAIL;
            }
            t.fade_pending = true;
        }
        delete old;
    }
    t.voices[slot] = new_voice;
    t.voice_age[slot] = ++age_counter_;
}

Voice* Mixer::primary_voice(int i) {
    LockGuard _g(*this);
    auto& t = tracks_[i];
    Voice* best = nullptr;
    uint32_t best_age = 0;
    for (int s = 0; s < TRACK_POLY; ++s) {
        if (t.voices[s] && t.voices[s]->active() && t.voice_age[s] >= best_age) {
            best = t.voices[s];
            best_age = t.voice_age[s];
        }
    }
    return best;
}

void Mixer::note_off_all(int i) {
    LockGuard _g(*this);
    auto& t = tracks_[i];
    for (int s = 0; s < TRACK_POLY; ++s) {
        if (t.voices[s]) t.voices[s]->note_off();
    }
}

void Mixer::cut_all(int i) {
    LockGuard _g(*this);
    auto& t = tracks_[i];
    for (int s = 0; s < TRACK_POLY; ++s) {
        if (t.voices[s]) t.voices[s]->cut();
    }
}

void Mixer::clear_voices(int i) {
    LockGuard _g(*this);
    auto& t = tracks_[i];
    for (int s = 0; s < TRACK_POLY; ++s) {
        if (t.voices[s]) {
            delete t.voices[s];
            t.voices[s] = nullptr;
            t.voice_age[s] = 0;
        }
    }
}

// === resample/bounce capture ===
void Mixer::start_resample(std::size_t max_frames) {
    // allocate a stereo buffer (interleaved L/R)
    resample_buf_.assign(max_frames * 2, 0);
    resample_max_ = max_frames;
    resample_pos_ = 0;
    resample_active_ = true;
}
void Mixer::stop_resample() {
    resample_active_ = false;
}

// === per-track DSP fx (MG + filter + bitcrush + downsample) ===
// in/out - the same scratch buffer (interleaved L/R q15)
static void apply_track_dsp(TrackState& t, fx::q15* buf, std::size_t frames) {
    // param slew: one-pole cutoff/resonance toward their targets once per chunk
    // (tau ~30ms). kaoss drags used to step the filter at UI rate = zipper noise.
    {
        int32_t dc = (int32_t)t.cutoff - t.cutoff_sm;
        t.cutoff_sm = (fx::q15)((dc > -4 && dc < 4) ? t.cutoff : t.cutoff_sm + (dc >> 2));
        int32_t dr = (int32_t)t.resonance - t.reso_sm;
        t.reso_sm = (fx::q15)((dr > -4 && dr < 4) ? t.resonance : t.reso_sm + (dr >> 2));
    }
    const fx::q15 cut = t.cutoff_sm;
    const fx::q15 res = t.reso_sm;

    const bool need_mg = (t.mg_to_cutoff != 0 || t.mg_to_vca != 0);
    const bool has_filter_changes = (t.filter_type != dsp::FilterType::Off) &&
                                     (cut < fx::Q15_ONE - 64 ||
                                      res > 64 ||
                                      t.filter_type != dsp::FilterType::LPF ||
                                      (need_mg && t.mg_to_cutoff != 0));
    bool need_filter   = has_filter_changes;
    bool need_bitcrush = (t.bits < 16);
    bool need_ds       = (t.downsample > 1);

    if (!need_filter && !need_bitcrush && !need_ds && !need_mg) return;

    // MG setup: set the rate (at sr=32000), we'll update it in the loop
    if (need_mg) {
        t.mg.set_rate_norm(t.mg_rate, 32000);
    }

    // filter - type and base params. set_params() is recomputed in the loop on MG modulation
    if (need_filter) {
        t.filter_l.type = t.filter_type;
        t.filter_r.type = t.filter_type;
        if (!need_mg || t.mg_to_cutoff == 0) {
            t.filter_l.set_params(cut, res);
            t.filter_r.set_params(cut, res);
        }
    }

    int crush_shift = 16 - t.bits;
    if (crush_shift < 0) crush_shift = 0;
    int32_t crush_mask = (int32_t)(0xFFFFFFFFu << crush_shift);

    // choose which MG shape to route to the targets (for now one is selected by mg_wave)
    const uint8_t wave_sel = t.mg_wave & 3;

    // to avoid calling set_params on every sample - recompute the SVF coeffs once every N samples
    // (for an LFO at frequencies up to ~20Hz this is more than smooth enough)
    constexpr int CUTOFF_UPDATE_RATE = 32;
    int coeff_counter = 0;

    for (std::size_t i = 0; i < frames; ++i) {
        int32_t sl = (int32_t)buf[i*2 + 0];
        int32_t sr = (int32_t)buf[i*2 + 1];

        // compute the MG output (all 4 shapes)
        fx::q15 mg_out = 0;
        if (need_mg) {
            dsp::MgOut mo = t.mg.step();
            switch (wave_sel) {
                case 0: mg_out = mo.tri; break;
                case 1: mg_out = mo.saw; break;
                case 2: mg_out = mo.sqr; break;
                case 3: mg_out = mo.sh;  break;
            }
        }

        if (need_ds) {
            if (t.ds_counter == 0) {
                t.ds_held_l = (fx::q15)sl;
                t.ds_held_r = (fx::q15)sr;
            }
            sl = (int32_t)t.ds_held_l;
            sr = (int32_t)t.ds_held_r;
            t.ds_counter = (t.ds_counter + 1) % t.downsample;
        }

        if (need_filter) {
            // effective cutoff = base + mg * depth (signed)
            // mg_out [-32767..+32767], mg_to_cutoff [-32767..+32767]
            // mod = (mg_out * mg_to_cutoff) >> 15  - in the q15 range
            int32_t cutoff_eff = (int32_t)cut;
            if (need_mg && t.mg_to_cutoff != 0) {
                int32_t mod = ((int32_t)mg_out * (int32_t)t.mg_to_cutoff) >> 15;
                cutoff_eff += mod;
                if (cutoff_eff < 0)            cutoff_eff = 0;
                if (cutoff_eff > fx::Q15_ONE)  cutoff_eff = fx::Q15_ONE;
                // recompute coeffs once every CUTOFF_UPDATE_RATE samples
                if (coeff_counter == 0) {
                    t.filter_l.set_params((fx::q15)cutoff_eff, res);
                    t.filter_r.set_params((fx::q15)cutoff_eff, res);
                }
                coeff_counter = (coeff_counter + 1) % CUTOFF_UPDATE_RATE;
            }
            sl = (int32_t)t.filter_l.step((fx::q15)sl);
            sr = (int32_t)t.filter_r.step((fx::q15)sr);
        }

        // MG -> VCA (amplitude modulation) - do it RIGHT AWAY (fast, a multiply)
        if (need_mg && t.mg_to_vca != 0) {
            // amp = 1 + (mg_out * depth) >> 15, clamp [0..2]
            // at depth=ONE and mg_out=+1: amp=2 (2x boost)
            // at depth=ONE and mg_out=-1: amp=0 (silence)
            int32_t mod = ((int32_t)mg_out * (int32_t)t.mg_to_vca) >> 15;
            int32_t amp = fx::Q15_ONE + mod;
            if (amp < 0) amp = 0;
            if (amp > fx::Q15_ONE * 2) amp = fx::Q15_ONE * 2;
            sl = (sl * amp) >> 15;
            sr = (sr * amp) >> 15;
            if (sl >  32767) sl =  32767;
            if (sl < -32768) sl = -32768;
            if (sr >  32767) sr =  32767;
            if (sr < -32768) sr = -32768;
        }

        if (need_bitcrush) {
            sl = sl & crush_mask;
            sr = sr & crush_mask;
            if (sl >  32767) sl =  32767;
            if (sl < -32768) sl = -32768;
            if (sr >  32767) sr =  32767;
            if (sr < -32768) sr = -32768;
        }

        buf[i*2 + 0] = (fx::q15)sl;
        buf[i*2 + 1] = (fx::q15)sr;
    }
}

void Mixer::render(fx::q15* out, std::size_t frames) {
    LockGuard _g(*this);
    std::memset(out, 0, frames * 2 * sizeof(fx::q15));

    std::size_t remaining = frames;
    fx::q15* dst = out;

    while (remaining > 0) {
        std::size_t chunk = remaining > 256 ? 256 : remaining;

        // clear all q31 accumulators of this chunk
        std::memset(del_bus_l_, 0, chunk * sizeof(fx::q31));
        std::memset(del_bus_r_, 0, chunk * sizeof(fx::q31));
        std::memset(rev_bus_l_, 0, chunk * sizeof(fx::q31));
        std::memset(rev_bus_r_, 0, chunk * sizeof(fx::q31));
        std::memset(master_l_, 0, chunk * sizeof(fx::q31));
        std::memset(master_r_, 0, chunk * sizeof(fx::q31));

        for (auto& t : tracks_) {
            // per-track scope keeps scrolling even when silent - write flatline
            // and skip. (same decimation rate as the active path: every 4th frame)
            auto tscope_flat = [&t, chunk]() {
                for (std::size_t i = 0; i < chunk; i += 4) {
                    t.tscope[t.tscope_pos] = 0;
                    t.tscope_pos = (t.tscope_pos + 1) % TrackState::TSCOPE_SIZE;
                }
            };
            if (t.muted) { tscope_flat(); continue; }

            // check that there is at least one voice
            bool any_voice = false;
            for (int s = 0; s < TRACK_POLY; ++s) {
                if (t.voices[s]) { any_voice = true; break; }
            }
            if (!any_voice && !t.fade_pending) { tscope_flat(); continue; }

            // SUM the voices into the q31 accumulator + count active ones
            std::memset(voice_acc_, 0, chunk * 2 * sizeof(fx::q31));
            // steal fade-tail: mix in the dying voice's last 2ms (see replace_voice)
            if (t.fade_pending) {
                std::size_t n = TrackState::FADE_TAIL * 2;
                if (n > chunk * 2) n = chunk * 2;
                for (std::size_t i = 0; i < n; ++i) voice_acc_[i] += t.fade_tail[i];
                std::memset(t.fade_tail, 0, sizeof(t.fade_tail));
                t.fade_pending = false;
            }
            int active_count = 0;
            for (int s = 0; s < TRACK_POLY; ++s) {
                if (!t.voices[s]) continue;
                bool alive = t.voices[s]->render(voice_buf_, chunk);
                for (std::size_t i = 0; i < chunk * 2; ++i) {
                    voice_acc_[i] += (fx::q31)voice_buf_[i];
                }
                if (alive) ++active_count;
                if (!alive) {
                    delete t.voices[s];
                    t.voices[s] = nullptr;
                    t.voice_age[s] = 0;
                }
            }
            // dynamic gain: 1 voice = full, 2 = 0.7, 3 = 0.58, 4 = 0.5 (1/sqrt(N) approximation)
            // IMPORTANT: scale-then-clip and NOT clip-then-scale!
            // otherwise voice_acc=+/-130000 would hard-clip to +/-32767 BEFORE the gain -> the whole point of the q31 acc is lost
            static const fx::q15 poly_gain[5] = {
                fx::Q15_ONE,                  // 0 (unrealistic but let it be)
                fx::Q15_ONE,                  // 1: 100%
                (fx::q15)(fx::Q15_ONE * 70 / 100),  // 2: 70% (~1/sqrt(2))
                (fx::q15)(fx::Q15_ONE * 58 / 100),  // 3: 58% (~1/sqrt(3))
                (fx::q15)(fx::Q15_ONE * 50 / 100),  // 4: 50% (1/sqrt(4))
            };
            int gi = (active_count > 4) ? 4 : active_count;
            fx::q15 vtgt = poly_gain[gi];
            fx::q15 vcur = t.vgain_cur;
            if (vcur == vtgt) {
                // stable voice count - flat gain (fast path)
                for (std::size_t i = 0; i < chunk * 2; ++i) {
                    // scale q31 directly (without an intermediate sat_q15!)
                    fx::q31 v = (vtgt == fx::Q15_ONE)
                        ? voice_acc_[i]
                        : (fx::q31)(((int64_t)voice_acc_[i] * vtgt) >> 15);
                    scratch_[i] = fx::soft_clip_q31(v);
                }
            } else {
                // voice count changed - ramp the gain linearly across the chunk.
                // an instant 1.0<->0.7 step retriggered every chunk boundary was
                // THE moonlight artifact: +/-3dB jumps on ringing tails at 125Hz.
                const int32_t dv = (int32_t)vtgt - (int32_t)vcur;
                for (std::size_t f = 0; f < chunk; ++f) {
                    fx::q15 g = (fx::q15)((int32_t)vcur + dv * (int32_t)(f + 1) / (int32_t)chunk);
                    fx::q31 vl = (fx::q31)(((int64_t)voice_acc_[f*2 + 0] * g) >> 15);
                    fx::q31 vr = (fx::q31)(((int64_t)voice_acc_[f*2 + 1] * g) >> 15);
                    scratch_[f*2 + 0] = fx::soft_clip_q31(vl);
                    scratch_[f*2 + 1] = fx::soft_clip_q31(vr);
                }
                t.vgain_cur = vtgt;
            }

            apply_track_dsp(t, scratch_, chunk);

            fx::q15 l_gain = fx::mul_q15(fx::mul_q15(t.volume, t.mix_vol), fx::Q15_ONE - (t.pan > 0 ? t.pan : 0));
            fx::q15 r_gain = fx::mul_q15(fx::mul_q15(t.volume, t.mix_vol), fx::Q15_ONE + (t.pan < 0 ? t.pan : 0));

            // sidechain duck: dip this track's gain by duck_amt * envelope.
            // computed once per chunk (256 frames = 8ms) - staircase is inaudible
            // through the linear decay, and it keeps the hot loop untouched.
            if (t.duck_amt > 0 && duck_env_ > 0) {
                fx::q15 dip = fx::mul_q15(t.duck_amt, duck_env_);
                fx::q15 dgain = fx::Q15_ONE - dip;
                l_gain = fx::mul_q15(l_gain, dgain);
                r_gain = fx::mul_q15(r_gain, dgain);
            }

            fx::q15 peak = 0;
            for (std::size_t i = 0; i < chunk; ++i) {
                fx::q15 sample_l = fx::mul_q15(scratch_[i*2 + 0], l_gain);
                fx::q15 sample_r = fx::mul_q15(scratch_[i*2 + 1], r_gain);
                // per-track scope (mono, every 4th frame - matches the master scope rate)
                if ((i & 3) == 0) {
                    t.tscope[t.tscope_pos] = (fx::q15)(((int32_t)sample_l + sample_r) / 2);
                    t.tscope_pos = (t.tscope_pos + 1) % TrackState::TSCOPE_SIZE;
                }
                // peak meter (abs max of both channels)
                fx::q15 al = sample_l < 0 ? (fx::q15)-sample_l : sample_l;
                fx::q15 ar = sample_r < 0 ? (fx::q15)-sample_r : sample_r;
                if (al > peak) peak = al;
                if (ar > peak) peak = ar;
                // sends: the clean pan'ed signal into each bus (q31 - doesn't clip from 8 tracks)
                if (t.send_del > 0) {
                    del_bus_l_[i] += fx::mul_q15(sample_l, t.send_del);
                    del_bus_r_[i] += fx::mul_q15(sample_r, t.send_del);
                }
                if (t.send_rev > 0) {
                    rev_bus_l_[i] += fx::mul_q15(sample_l, t.send_rev);
                    rev_bus_r_[i] += fx::mul_q15(sample_r, t.send_rev);
                }
                // dry -> master q31 acc (without hard-clip; the sum of 8 tracks up to ~262k survives in q31)
                master_l_[i] += sample_l;
                master_r_[i] += sample_r;
            }
            // meter: instant attack, slow decay (~-6dB per 256-frame chunk)
            if (peak > t.meter) t.meter = peak;
            else t.meter = (fx::q15)((int32_t)t.meter * 7 / 8);
        }

        // === FX buses → delay (ping-pong) + reverb, now fed independently ===
        // buses can be larger than q15 (several tracks with send). compress with headroom
        // instead of a hard clip: >>2 = -12dB at the delay/reverb input

        // auto-bypass: if the whole chunk in rev_bus = 0, don't process reverb BUT keep the tail alive
        // (the reverb tail sustains itself via feedback - cutting it on the first silent chunk = clipping the reverb with a cliff)
        // the constant 64000 ~= 2sec @ 32kHz - enough for the RT60 of a fat reverb
        bool rev_bus_silent = true;
        for (std::size_t i = 0; i < chunk; ++i) {
            if (rev_bus_l_[i] != 0 || rev_bus_r_[i] != 0) { rev_bus_silent = false; break; }
        }
        if (!rev_bus_silent) {
            reverb_tail_frames_ = 64000;
        } else if (reverb_tail_frames_ > 0) {
            // decrement by the chunk size (keep reverb on)
            if (reverb_tail_frames_ > chunk) reverb_tail_frames_ -= chunk;
            else reverb_tail_frames_ = 0;
            rev_bus_silent = false;     // keep processing reverb with zero input (the tail decays on its own)
        }

        for (std::size_t i = 0; i < chunk; ++i) {
            fx::q15 in_dl = fx::sat_q15(del_bus_l_[i] >> 2);   // -12dB headroom into delay
            fx::q15 in_dr = fx::sat_q15(del_bus_r_[i] >> 2);
            fx::q15 in_rl = fx::sat_q15(rev_bus_l_[i] >> 2);   // -12dB headroom into reverb
            fx::q15 in_rr = fx::sat_q15(rev_bus_r_[i] >> 2);

            fx::q15 wet_l = 0, wet_r = 0;

            {
                // === DELAY (ping-pong) ===
                std::size_t read_pos = (delay_pos + DELAY_BUF - delay_time) % DELAY_BUF;
                fx::q15 d_l = delay_l[read_pos];
                fx::q15 d_r = delay_r[read_pos];

                fx::q15 fb_l = fx::mul_q15(d_r, delay_feedback);
                fx::q15 fb_r = fx::mul_q15(d_l, delay_feedback);

                delay_l[delay_pos] = fx::sat_q15((fx::q31)in_dl + fb_l);
                delay_r[delay_pos] = fx::sat_q15((fx::q31)in_dr + fb_r);

                wet_l = fx::mul_q15(d_l, delay_wet);
                wet_r = fx::mul_q15(d_r, delay_wet);

                delay_pos = (delay_pos + 1) % DELAY_BUF;
            }

            if (!rev_bus_silent) {
                // === REVERB from its own bus ===
                fx::q15 rv_l, rv_r;
                reverb.process(in_rl, in_rr, rv_l, rv_r);
                wet_l = fx::sat_q15((fx::q31)wet_l + fx::mul_q15(rv_l, reverb_wet));
                wet_r = fx::sat_q15((fx::q31)wet_r + fx::mul_q15(rv_r, reverb_wet));
            }

            // add wet into the master q31 acc (alongside dry)
            master_l_[i] += wet_l;
            master_r_[i] += wet_r;
        }

        // === final fold q31 -> q15 with soft-clip ===
        // it used to be `>> 2` (-12dB) for hard sat headroom - now soft_clip handles it itself.
        // without the shift - a single voice at full volume, many voices/tracks compress smoothly.
        for (std::size_t i = 0; i < chunk; ++i) {
            dst[i*2 + 0] = fx::soft_clip_q31(master_l_[i]);
            dst[i*2 + 1] = fx::soft_clip_q31(master_r_[i]);
        }

        // sidechain duck envelope: linear decay to zero over duck_rel_frames
        if (duck_env_ > 0) {
            uint32_t rel = duck_rel_frames < 256 ? 256 : duck_rel_frames;
            int32_t step = (int32_t)((int64_t)fx::Q15_ONE * chunk / rel);
            if (step < 1) step = 1;
            duck_env_ = (duck_env_ > step) ? (fx::q15)(duck_env_ - step) : 0;
        }

        remaining -= chunk;
        dst += chunk * 2;
    }

    // === master clipper + DC blocker ===
    constexpr int32_t DC_ALPHA = 32669;   // 0.997 * 32768

    for (std::size_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            std::size_t off = i * 2 + ch;
            int32_t v = (master != fx::Q15_ONE) ? fx::mul_q15(out[off], master) : out[off];

            constexpr int32_t THR = 18000;
            if (v > THR) {
                int32_t over = v - THR;
                int32_t range = 32768 - THR;
                int32_t r = (over * 32768) / range;
                if (r > 32768) r = 32768;
                int32_t r2 = (r * r) >> 15;
                int32_t shaped = r - ((r2 * r) / 3 >> 15);
                if (shaped > 32767) shaped = 32767;
                v = THR + (shaped * range) / 32768;
            } else if (v < -THR) {
                int32_t over = -THR - v;
                int32_t range = 32768 - THR;
                int32_t r = (over * 32768) / range;
                if (r > 32768) r = 32768;
                int32_t r2 = (r * r) >> 15;
                int32_t shaped = r - ((r2 * r) / 3 >> 15);
                if (shaped > 32767) shaped = 32767;
                v = -THR - (shaped * range) / 32768;
            }
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;

            int32_t* dc_x = (ch == 0) ? &dc_x_l_ : &dc_x_r_;
            int32_t* dc_y = (ch == 0) ? &dc_y_l_ : &dc_y_r_;
            int32_t y = v - *dc_x + ((*dc_y * DC_ALPHA) >> 15);
            *dc_x = v;
            *dc_y = y;
            if (y > 32767) y = 32767;
            if (y < -32768) y = -32768;

            out[off] = (fx::q15)y;
        }
    }

    // scope buffer (mono = (L+R)/2)
    for (std::size_t i = 0; i < frames; i += 4) {
        int32_t mono = ((int32_t)out[i*2] + (int32_t)out[i*2 + 1]) / 2;
        scope[scope_write_pos] = (fx::q15)mono;
        scope_write_pos = (scope_write_pos + 1) % SCOPE_SIZE;
    }

    // resample/bounce - record master output into an internal buffer
    if (resample_active_ && resample_pos_ < resample_max_) {
        std::size_t avail = resample_max_ - resample_pos_;
        std::size_t copy  = (frames < avail) ? frames : avail;
        std::memcpy(resample_buf_.data() + resample_pos_ * 2,
                    out, copy * 2 * sizeof(fx::q15));
        resample_pos_ += copy;
        if (resample_pos_ >= resample_max_) resample_active_ = false;     // auto-stop on overflow
    }
}

} // namespace trackr::audio
