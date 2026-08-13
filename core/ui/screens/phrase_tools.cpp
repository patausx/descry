// App: phrase transforms/generators popup, range capture and touch handling.
#include "../app.h"
#include "../ui_internal.h"
#include "../../sequencer/phrase_gen.h"
#include "../../sequencer/fx.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace trackr::ui {

namespace {
    constexpr int PT_X = 4, PT_Y = 92, PT_W = 312, PT_ROW_H = 11;
    constexpr int PT_ROWS = 11, PT_COL_W = PT_W / 2;
    static const char* const kPhraseToolNames[] = {
        "ROTATE UP", "ROTATE DOWN", "REVERSE",
        "TRANSPOSE +1", "TRANSPOSE -1", "OCTAVE +12", "OCTAVE -12",
        "VELOCITY +8", "VELOCITY -8", "VELOCITY RAMP UP", "VELOCITY RAMP DOWN",
        "EUCLIDEAN", "DENSITY GATE", "HUMANIZE", "RATCHET FILL",
        "MUTATE NOTES", "RANDOM NOTES", "CHANCE SPREAD", "EVERY CYCLE",
    };
    constexpr int PT_COUNT = (int)(sizeof(kPhraseToolNames) / sizeof(kPhraseToolNames[0]));
    constexpr int PT_GEN_FIRST = 11;

    int gen_min(int tool, int range_len) {
        if (tool == 11) return 0;              // Euclidean pulses
        if (tool == 15) return 0;              // mutate probability
        if (tool == 16) return 1;              // random-note semitone span
        if (tool == 18) return 2;              // EVN cycle
        (void)range_len;
        return 0;
    }
    int gen_max(int tool, int range_len) {
        if (tool == 11) return range_len;       // Euclidean pulses
        if (tool == 13) return 63;              // velocity jitter
        if (tool == 16) return 48;              // note span
        if (tool == 18) return 15;              // EVN nibble
        return 100;                             // probability / density / chance
    }
    int gen_default(int tool, int range_len) {
        switch (tool) {
            case 11: return (range_len + 1) / 2;
            case 12: return 70;
            case 13: return 8;
            case 14: return 35;
            case 15: return 25;
            case 16: return 12;
            case 17: return 35;
            case 18: return 4;
            default: return 0;
        }
    }
    const char* gen_unit(int tool) {
        switch (tool) {
            case 11: return "PULSES";
            case 13: return "VEL +/-";
            case 16: return "SPAN ST";
            case 18: return "PASSES";
            case 12: return "KEEP %";
            case 14: case 15: return "AMOUNT %";
            case 17: return "MIN %";
            default: return "AMOUNT";
        }
    }
}

void App::open_phrase_tools() {
    const auto& ph = project_.phrases[cur_phrase_];
    phrase_tools_range_ = sel_mode_;
    if (sel_mode_) {
        phrase_tools_lo_ = std::min(sel_anchor_, cursor_row_);
        phrase_tools_hi_ = std::max(sel_anchor_, cursor_row_);
    } else {
        phrase_tools_lo_ = 0;
        phrase_tools_hi_ = seq::phrase_len(ph) - 1;
    }
    phrase_tools_on_ = true;
    phrase_tools_closing_ = false;
    fx_help_closing_ = false;
    phrase_tool_config_ = false;
    fx_help_ = false;
}

void App::apply_phrase_tool(PhraseTool tool) {
    int lo = phrase_tools_lo_, hi = phrase_tools_hi_;
    if (lo < 0) lo = 0;
    if (hi >= seq::PHRASE_STEPS) hi = seq::PHRASE_STEPS - 1;
    if (lo > hi) return;

    auto& ph = project_.phrases[cur_phrase_];
    seq::PhraseStep before[seq::PHRASE_STEPS];
    for (int r = lo; r <= hi; ++r) before[r] = ph.steps[r];

    const int n = hi - lo + 1;
    switch (tool) {
        case PhraseTool::RotateUp:
            if (n > 1) {
                seq::PhraseStep first = ph.steps[lo];
                for (int r = lo; r < hi; ++r) ph.steps[r] = ph.steps[r + 1];
                ph.steps[hi] = first;
            }
            break;
        case PhraseTool::RotateDown:
            if (n > 1) {
                seq::PhraseStep last = ph.steps[hi];
                for (int r = hi; r > lo; --r) ph.steps[r] = ph.steps[r - 1];
                ph.steps[lo] = last;
            }
            break;
        case PhraseTool::Reverse:
            for (int a = lo, b = hi; a < b; ++a, --b) std::swap(ph.steps[a], ph.steps[b]);
            break;
        case PhraseTool::TransposeUp:
        case PhraseTool::TransposeDown:
        case PhraseTool::OctaveUp:
        case PhraseTool::OctaveDown: {
            int delta = tool == PhraseTool::TransposeUp ? 1
                      : tool == PhraseTool::TransposeDown ? -1
                      : tool == PhraseTool::OctaveUp ? 12 : -12;
            for (int r = lo; r <= hi; ++r) {
                if (ph.steps[r].note == seq::EMPTY) continue;
                ph.steps[r].note = (uint8_t)clamp_int((int)ph.steps[r].note + delta, 0, 127);
            }
            break;
        }
        case PhraseTool::VelocityUp:
        case PhraseTool::VelocityDown: {
            int delta = tool == PhraseTool::VelocityUp ? 8 : -8;
            for (int r = lo; r <= hi; ++r) {
                if (ph.steps[r].note == seq::EMPTY) continue;
                bump_clamped(ph.steps[r].velocity, delta, 1, 127);
            }
            break;
        }
        case PhraseTool::RampUp:
        case PhraseTool::RampDown:
            for (int r = lo; r <= hi; ++r) {
                if (ph.steps[r].note == seq::EMPTY) continue;
                int pos = r - lo;
                int vel = n <= 1 ? 127 : 32 + pos * 95 / (n - 1);
                if (tool == PhraseTool::RampDown) vel = 159 - vel;
                ph.steps[r].velocity = (uint8_t)vel;
            }
            break;
        default: {
            seq::PhraseGenOptions opt;
            opt.seed = phrase_tool_seed_;
            opt.amount = phrase_tool_amount_;
            opt.scale_type = project_.song.scale_type;
            opt.scale_root = project_.song.scale_root;
            opt.default_note = last_note_entered_;
            opt.instrument = cur_inst_;
            // Prefer a complete playable row as the Euclidean/random pitch centre
            // and instrument template; otherwise use the editor's sticky values.
            for (int r = lo; r <= hi; ++r) {
                if (ph.steps[r].note != seq::EMPTY) opt.default_note = ph.steps[r].note;
                if (ph.steps[r].note != seq::EMPTY && ph.steps[r].instrument != seq::EMPTY) {
                    opt.instrument = ph.steps[r].instrument;
                    break;
                }
            }
            const int gi = (int)tool - (int)PhraseTool::Euclidean;
            if (gi >= 0 && gi <= (int)seq::PhraseGenOp::Every)
                seq::apply_phrase_generator(ph, lo, hi, (seq::PhraseGenOp)gi, opt);
            break;
        }
    }

    // A transform is one user action even though the compact undo ring stores
    // per-row deltas. Record only byte-different rows and group them.
    bool changed = false;
    uint16_t changed_mask = 0;
    undo_.begin_group();
    for (int r = lo; r <= hi; ++r) {
        if (std::memcmp(&before[r], &ph.steps[r], sizeof(seq::PhraseStep)) == 0) continue;
        changed = true;
        changed_mask |= (uint16_t)(1u << r);
        seq::EditRecord::Payload b{}, a{};
        b.step = before[r];
        a.step = ph.steps[r];
        undo_.record(seq::EditKind::Step, cur_phrase_, (uint16_t)r, b, a, frame_, 0);
    }
    undo_.end_group();
    if (changed) {
        edit_flash_frame_ = frame_;
        mark_project_dirty();
        start_phrase_motion(changed_mask, 1);
    }
    begin_overlay_close(phrase_tools_closing_);
    phrase_tool_config_ = false;
    sel_mode_ = false;
}

void App::draw_phrase_tools(Draw& d) {
    static_assert(PT_COUNT == static_cast<int>(PhraseTool::COUNT));
    constexpr int H = 146;
    d.rect(PT_X - 2, PT_Y - 2, PT_W + 4, H + 4, pal::BG_HI);
    d.rect(PT_X, PT_Y, PT_W, H, pal::BG);
    if (phrase_tools_closing_) {
        uint32_t ca = frame_ - overlay_close_frame_;
        uint8_t a = (uint8_t)clamp_int((int)ca * 52, 0, 210);
        d.rect(PT_X, PT_Y, PT_W, H, with_alpha(pal::BG, a));
        int in = (int)motion_in(ca, 4) * 18 / 255;
        d.corner_brackets(PT_X + in, PT_Y + in / 2, PT_W - in * 2, H - in,
                          with_alpha(pal::CURSOR, (uint8_t)(220 - a)), 6, 1);
        return;
    }

    char title[48];
    std::snprintf(title, sizeof(title), "PHRASE TOOLS %02X-%02X%s",
                  phrase_tools_lo_, phrase_tools_hi_, phrase_tools_range_ ? " SEL" : "");
    d.text(PT_X + 5, PT_Y + 4, title, pal::HEADER);

    if (phrase_tool_config_) {
        const int tool = phrase_tool_sel_;
        d.text(PT_X + 5, PT_Y + 20, kPhraseToolNames[tool], pal::CURSOR);
        char b[48];
        std::snprintf(b, sizeof(b), "%s %d   SEED %08lX", gen_unit(tool), phrase_tool_amount_,
                      (unsigned long)phrase_tool_seed_);
        d.text(PT_X + 12, PT_Y + 34, b, pal::FG);

        // Live dry-run: Phrase is only 16 compact steps, so a stack copy + the
        // pure deterministic generator costs no heap and never touches playback.
        seq::Phrase preview = project_.phrases[cur_phrase_];
        seq::PhraseGenOptions opt;
        opt.seed = phrase_tool_seed_;
        opt.amount = phrase_tool_amount_;
        opt.scale_type = project_.song.scale_type;
        opt.scale_root = project_.song.scale_root;
        opt.default_note = last_note_entered_;
        opt.instrument = cur_inst_;
        for (int r = phrase_tools_lo_; r <= phrase_tools_hi_; ++r) {
            const auto& st = preview.steps[r];
            if (st.note != seq::EMPTY) opt.default_note = st.note;
            if (st.note != seq::EMPTY && st.instrument != seq::EMPTY) {
                opt.instrument = st.instrument;
                break;
            }
        }
        const int gi = tool - (int)PhraseTool::Euclidean;
        if (gi >= 0 && gi <= (int)seq::PhraseGenOp::Every)
            seq::apply_phrase_generator(preview, phrase_tools_lo_, phrase_tools_hi_,
                                        (seq::PhraseGenOp)gi, opt);
        constexpr int PV_X = PT_X + 12, PV_Y = PT_Y + 52, PV_W = 288, PV_H = 28;
        d.rect(PV_X, PV_Y, PV_W, PV_H, pal::PANEL);
        for (int r = 0; r < seq::PHRASE_STEPS; ++r) {
            const int x = PV_X + r * (PV_W / seq::PHRASE_STEPS);
            const auto& now = preview.steps[r];
            const auto& old = project_.phrases[cur_phrase_].steps[r];
            const bool in_range = r >= phrase_tools_lo_ && r <= phrase_tools_hi_;
            const bool hit = now.note != seq::EMPTY;
            const bool changed = std::memcmp(&now, &old, sizeof(now)) != 0;
            d.rect(x, PV_Y, 1, PV_H, with_alpha(pal::GRID, 80));
            if (!in_range) continue;
            if (hit) {
                int bh = 4 + (int)now.velocity * (PV_H - 7) / 127;
                Color pc = changed ? pal::CURSOR : pal::PLAY;
                d.rect(x + 3, PV_Y + PV_H - 2 - bh, 11, bh, with_alpha(pc, 190));
                // Ratchets read as subdivisions instead of a generic solid bar.
                for (int f = 0; f < 3; ++f) if (now.fx[f].cmd == seq::fx_cmd::RTG) {
                    int div = clamp_int(6 / clamp_int(now.fx[f].value, 1, 6), 1, 6);
                    for (int k = 1; k < div; ++k)
                        d.rect(x + 3 + k * 11 / div, PV_Y + PV_H - 2 - bh, 1, bh, pal::BG);
                }
            } else if (old.note != seq::EMPTY) {
                d.rect(x + 4, PV_Y + PV_H / 2, 9, 1, pal::RECORD);
            }
        }
        d.text(PT_X + 12, PT_Y + 86, "LR amount  X/Y +/-8  UD seed", pal::FG_DIM);
        d.text(PT_X + 12, PT_Y + 102, "A=APPLY  B=BACK  preview is non-destructive", pal::HEADER);
        return;
    }

    d.text(PT_X + PT_W - 82, PT_Y + 4, "A=OPEN B=X", pal::FG_DIM);
    const int list_y = PT_Y + 18;
    for (int i = 0; i < PT_COUNT; ++i) {
        const int col = i / PT_ROWS;
        const int row = i % PT_ROWS;
        const int x = PT_X + col * PT_COL_W;
        const int y = list_y + row * PT_ROW_H;
        const bool sel = i == phrase_tool_sel_;
        if (sel) d.rect(x + 2, y - 1, PT_COL_W - 4, PT_ROW_H, with_alpha(pal::CURSOR, 90));
        const char* label = kPhraseToolNames[i];
        const int len = (int)std::strlen(label);
        const int tx = x + (PT_COL_W - len * 6) / 2;
        const int ty = y + (PT_ROW_H - 8) / 2;
        d.text(tx, ty, label, sel ? pal::FG : (i >= PT_GEN_FIRST ? pal::FG_HEX : pal::FG_DIM));
    }
}

bool App::phrase_tools_touch(int x, int y) {
    if (x < PT_X || x >= PT_X + PT_W) return false;
    if (phrase_tool_config_) {
        if (y >= PT_Y + 38 && y < PT_Y + 65) {
            int delta = x < PT_X + PT_W / 2 ? -1 : 1;
            phrase_tool_amount_ = clamp_int(phrase_tool_amount_ + delta,
                gen_min(phrase_tool_sel_, phrase_tools_hi_ - phrase_tools_lo_ + 1),
                gen_max(phrase_tool_sel_, phrase_tools_hi_ - phrase_tools_lo_ + 1));
            return true;
        }
        if (y >= PT_Y + 65 && y < PT_Y + 90) {
            phrase_tool_seed_ += x < PT_X + PT_W / 2 ? (uint32_t)-1 : 1u;
            return true;
        }
        if (y >= PT_Y + 112) { apply_phrase_tool((PhraseTool)phrase_tool_sel_); return true; }
        return true;
    }
    const int list_y = PT_Y + 18;
    if (y < list_y || y >= list_y + PT_ROWS * PT_ROW_H) return false;
    const int col = (x - PT_X) / PT_COL_W;
    const int row = (y - list_y) / PT_ROW_H;
    const int i = col * PT_ROWS + row;
    if (i < 0 || i >= PT_COUNT) return false;
    if (i == phrase_tool_sel_) {
        if (i >= PT_GEN_FIRST) {
            phrase_tool_config_ = true;
            phrase_tool_amount_ = gen_default(i, phrase_tools_hi_ - phrase_tools_lo_ + 1);
        } else apply_phrase_tool((PhraseTool)i);
    } else phrase_tool_sel_ = i;
    return true;
}

} // namespace trackr::ui
