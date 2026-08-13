#include "phrase_gen.h"
#include "fx.h"
#include "scale.h"
#include <algorithm>
#include <cstring>

namespace trackr::seq {
namespace {

class GenRng {
public:
    explicit GenRng(uint32_t seed) : state_(seed ? seed : 0x6D2B79F5u) {}
    uint32_t next() {
        uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }
    int range(int n) { return n > 0 ? (int)(next() % (uint32_t)n) : 0; }
    bool percent(int p) {
        if (p <= 0) return false;
        if (p >= 100) return true;
        return range(100) < p;
    }
private:
    uint32_t state_;
};

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

FxCmd* find_fx(PhraseStep& step, uint8_t cmd) {
    // Player processes slots left-to-right, so the last duplicate is effective.
    // Update that one rather than creating another conflicting command.
    FxCmd* found = nullptr;
    for (auto& fx : step.fx) if (fx.cmd == cmd) found = &fx;
    return found;
}

FxCmd* ensure_fx(PhraseStep& step, uint8_t cmd) {
    if (FxCmd* fx = find_fx(step, cmd)) return fx;
    for (auto& fx : step.fx) {
        if (fx.cmd == fx_cmd::NONE) {
            fx.cmd = cmd;
            fx.value = 0;
            return &fx;
        }
    }
    return nullptr; // never destroy an existing musical instruction
}

int random_scale_note(GenRng& rng, uint8_t type, uint8_t root, int center, int span) {
    center = clampi(center, 0, 127);
    span = clampi(span, 1, 48);
    int notes[97];
    int count = 0;
    const int lo = std::max(0, center - span);
    const int hi = std::min(127, center + span);
    for (int n = lo; n <= hi; ++n)
        if (scale_has(type, root, n)) notes[count++] = n;
    return count ? notes[rng.range(count)] : scale_snap(type, root, center);
}

int moved_scale_note(GenRng& rng, uint8_t type, uint8_t root, int note) {
    int moves = 1 + rng.range(3);
    int dir = rng.range(2) ? 1 : -1;
    int out = note;
    for (int i = 0; i < moves; ++i) {
        int next = scale_step(type, root, out, dir);
        if (next == out) {
            dir = -dir;
            next = scale_step(type, root, out, dir);
        }
        out = next;
    }
    return out;
}

} // namespace

bool apply_phrase_generator(Phrase& phrase, int lo, int hi, PhraseGenOp op,
                            const PhraseGenOptions& options) {
    lo = clampi(lo, 0, PHRASE_STEPS - 1);
    hi = clampi(hi, 0, PHRASE_STEPS - 1);
    if (lo > hi) return false;

    PhraseStep before[PHRASE_STEPS];
    for (int r = lo; r <= hi; ++r) before[r] = phrase.steps[r];

    GenRng rng(options.seed);
    const int n = hi - lo + 1;
    const int amount = options.amount;

    switch (op) {
        case PhraseGenOp::Euclidean: {
            const int pulses = clampi(amount, 0, n);
            // Evenly distributed bucket/Bjorklund-equivalent pattern, rotated so
            // a non-empty rhythm always begins on the selection's first row.
            int bucket = n - pulses;
            for (int i = 0; i < n; ++i) {
                bucket += pulses;
                const bool hit = pulses > 0 && bucket >= n;
                if (hit) bucket -= n;
                auto& step = phrase.steps[lo + i];
                if (hit) {
                    if (step.note == EMPTY) {
                        step.note = (uint8_t)scale_snap(options.scale_type,
                                                        options.scale_root,
                                                        options.default_note);
                        step.velocity = 0x64;
                    }
                    // A sounding trigger requires note+instrument on the same row.
                    if (step.instrument == EMPTY) step.instrument = options.instrument;
                } else {
                    step.note = EMPTY;
                    step.instrument = EMPTY;
                    // Keep FX: a condition/filter command on a rest may be intentional.
                }
            }
            break;
        }
        case PhraseGenOp::Density: {
            const int keep = clampi(amount, 0, 100);
            for (int r = lo; r <= hi; ++r) {
                auto& step = phrase.steps[r];
                if (step.note != EMPTY && !rng.percent(keep)) {
                    step.note = EMPTY;
                    step.instrument = EMPTY;
                }
            }
            break;
        }
        case PhraseGenOp::Humanize: {
            const int depth = clampi(amount, 0, 63);
            for (int r = lo; r <= hi; ++r) {
                auto& step = phrase.steps[r];
                if (step.note == EMPTY) continue;
                const int delta = depth ? rng.range(depth * 2 + 1) - depth : 0;
                step.velocity = (uint8_t)clampi((int)step.velocity + delta, 1, 127);
                // Tracker timing cannot be negative. Add 0..2 ticks of micro-delay;
                // if all FX lanes are occupied, velocity still gets humanized.
                if (depth > 0) {
                    const uint8_t ticks = (uint8_t)rng.range(3);
                    if (ticks > 0) if (FxCmd* fx = ensure_fx(step, fx_cmd::DLY)) fx->value = ticks;
                }
            }
            break;
        }
        case PhraseGenOp::Ratchet: {
            const int probability = clampi(amount, 0, 100);
            for (int r = lo; r <= hi; ++r) {
                auto& step = phrase.steps[r];
                if (step.note == EMPTY || !rng.percent(probability)) continue;
                if (FxCmd* fx = ensure_fx(step, fx_cmd::RTG))
                    fx->value = (uint8_t)(1 + rng.range(3));
            }
            break;
        }
        case PhraseGenOp::Mutate: {
            const int probability = clampi(amount, 0, 100);
            for (int r = lo; r <= hi; ++r) {
                auto& step = phrase.steps[r];
                if (step.note == EMPTY || !rng.percent(probability)) continue;
                step.note = (uint8_t)moved_scale_note(rng, options.scale_type,
                                                      options.scale_root, step.note);
            }
            break;
        }
        case PhraseGenOp::RandomNotes: {
            const int span = clampi(amount, 1, 48);
            for (int r = lo; r <= hi; ++r) {
                auto& step = phrase.steps[r];
                step.note = (uint8_t)random_scale_note(rng, options.scale_type,
                                                       options.scale_root,
                                                       options.default_note, span);
                if (step.instrument == EMPTY) step.instrument = options.instrument;
                if (step.velocity == 0) step.velocity = 0x64;
            }
            break;
        }
        case PhraseGenOp::ChanceSpread: {
            const int min_percent = clampi(amount, 0, 100);
            for (int r = lo; r <= hi; ++r) {
                auto& step = phrase.steps[r];
                if (step.note == EMPTY) continue;
                if (FxCmd* fx = ensure_fx(step, fx_cmd::CHA)) {
                    const int pct = min_percent + rng.range(101 - min_percent);
                    fx->value = (uint8_t)(pct * 255 / 100);
                }
            }
            break;
        }
        case PhraseGenOp::Every: {
            const int cycle = clampi(amount, 2, 15);
            int hit = 0;
            for (int r = lo; r <= hi; ++r) {
                auto& step = phrase.steps[r];
                if (step.note == EMPTY) continue;
                if (FxCmd* fx = ensure_fx(step, fx_cmd::EVN)) {
                    const int pass = (hit++ % cycle) + 1;
                    fx->value = (uint8_t)((pass << 4) | cycle);
                }
            }
            break;
        }
    }

    for (int r = lo; r <= hi; ++r)
        if (std::memcmp(&before[r], &phrase.steps[r], sizeof(PhraseStep)) != 0) return true;
    return false;
}

} // namespace trackr::seq
