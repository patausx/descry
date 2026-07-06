// App: mod-table editor screen. Split out of app.cpp.
// Owns update_table + draw_table.
#include "../app.h"
#include "../ui_internal.h"
#include "../../sequencer/fx.h"
#include <cstdio>

namespace trackr::ui {

void App::update_table(const InputState& in) {
    constexpr int N_COLS = 6;   // 3 fx-slots × (cmd, val)
    if (in.up)    table_row_ = (table_row_ - 1 + seq::TABLE_ROWS) % seq::TABLE_ROWS;
    if (in.down)  table_row_ = (table_row_ + 1) % seq::TABLE_ROWS;
    if (in.left)  table_col_ = (table_col_ - 1 + N_COLS) % N_COLS;
    if (in.right) table_col_ = (table_col_ + 1) % N_COLS;

    // R+A/B = table speed (ticks per row, 1..16). the table used to fly at one
    // row per TICK with no brake - at groove 6 that's 6 rows per step.
    if (in.held_r && (in.a || in.b)) {
        uint8_t& spd = project_.table_speed[cur_table_];
        int v = (spd < 1) ? 1 : spd;      // 0 = legacy default = 1
        v += in.a ? 1 : -1;
        if (v < 1) v = 1;
        if (v > 16) v = 16;
        spd = (uint8_t)v;
        edit_flash_frame_ = frame_;
        dirty = true;
        return;                            // don't fall through to cell edit
    }

    int delta = 0;
    if (in.a) delta = +1;
    if (in.b) delta = -1;
    if (in.x) delta = +16;
    if (in.y) delta = -16;
    if (delta) {
        auto& row = project_.tables[cur_table_].rows[table_row_];
        int slot = table_col_ / 2;
        bool is_cmd = (table_col_ & 1) == 0;
        if (is_cmd) {
            // track-level fx that make MUSICAL sense at table tick rate:
            // vol/pan, filter cut/res, crush, sends, LFO rate/depths.
            // deliberately excluded: T (bpm per tick = chaos), Y (filter type
            // stepping = clicks), W (lfo wave per tick = noise). m8 allows all,
            // we curate. grouped: V A | F Q | B | S E G | L M N
            static const char fx_letters[] = "VAFQBSEGLMN";
            uint8_t cur = row.fx[slot].cmd;
            int idx = -1;
            int n = sizeof(fx_letters) - 1;
            if (cur != 0) for (int i = 0; i < n; ++i) if (fx_letters[i] == cur) { idx = i; break; }
            idx += (delta > 0 ? 1 : -1);
            if (idx < -1) idx = -1;
            if (idx >= n) idx = n - 1;
            row.fx[slot].cmd = (idx == -1) ? 0 : (uint8_t)fx_letters[idx];
        } else {
            int vmax = seq::fx_value_max(row.fx[slot].cmd);
            int v = (int)row.fx[slot].value + delta;
            if (v < 0) v = 0;
            if (v > vmax) v = vmax;
            row.fx[slot].value = (uint8_t)v;
        }
        edit_flash_frame_ = frame_;   // value-flash animation
        dirty = true;
    }

    // SELECT - assign this table to the current instrument AND fire a preview
    // note. NB the note itself won't run the table (tables tick inside the
    // player during playback) - the assignment is the real payload here.
    if (in.select_) {
        project_.instruments[cur_inst_].table_id = cur_table_;
        auto* v = project_.make_voice(cur_inst_);
        mixer_.replace_voice(0, v);
        if (v) v->note_on(60, 110);
        dirty = true;
    }
}

void App::draw_table(Draw& d) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "TABLE %02X", cur_table_);
    d.text(20, 22, buf, pal::HEADER);
    // speed (ticks per row): part of the table's identity, edited with R+A/B
    {
        uint8_t spd = project_.table_speed[cur_table_];
        if (spd < 1) spd = 1;
        std::snprintf(buf, sizeof(buf), "SPD%02d", spd);
        d.text(88, 22, buf, spd > 1 ? pal::CURSOR : pal::FG_DIM);
    }
    d.text(130, 22, "ASSIGNED:", pal::HEADER);

    // find which instruments use this table
    int xoff = 192;
    int found = 0;
    for (int i = 0; i < seq::MAX_INSTRUMENTS && found < 6; ++i) {
        if (project_.instruments[i].table_id == cur_table_ &&
            project_.instruments[i].type != seq::InstrumentType::None) {
            std::snprintf(buf, sizeof(buf), "%02X", i);
            d.text(xoff, 22, buf, pal::FG);
            xoff += 24;
            ++found;
        }
    }
    if (found == 0) d.text(192, 22, "(none)", pal::FG_DIM);

    // headers. tighter rows (11px) so 16 rows + the hint bar never collide:
    // rows span 48..224, the hint bar sits at 228 with an opaque backing.
    constexpr int Y0 = 36;
    constexpr int ROW_H = 11;
    constexpr int COL_X[] = {30, 70, 130, 190, 260, 320};
    d.text(COL_X[0], Y0, "##", pal::HEADER);
    d.text(COL_X[1], Y0, "FX1", pal::HEADER);
    d.text(COL_X[2], Y0, "FX2", pal::HEADER);
    d.text(COL_X[3], Y0, "FX3", pal::HEADER);

    // show the position of the active table if it's playing
    int playing_row = -1;
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        if (player_.track_state(t).table_active &&
            player_.track_state(t).table_id == cur_table_) {
            playing_row = player_.track_state(t).table_row;
            break;
        }
    }

    for (int r = 0; r < seq::TABLE_ROWS; ++r) {
        int y = Y0 + 12 + r * ROW_H;
        if (r & 1) d.rect(0, y - 1, 400, ROW_H, pal::BG_HI);
        if (r == playing_row) {
            // flowing gradient sweep - tables tick fast, the glow rides the rows
            d.rect(0, y - 1, 400, ROW_H, pal::PLAY_BG);
            beat_glow(d, 0, y - 1, 400, ROW_H, frame_ - step_change_frame_, pal::PLAY, 10);
            d.rect(0, y - 1, 3, ROW_H, pal::PLAY);
            d.rect(397, y - 1, 3, ROW_H, pal::PLAY);
        }

        d.hex2(COL_X[0], y, r, pal::FG_DIM);
        const auto& row = project_.tables[cur_table_].rows[r];
        for (int s = 0; s < 3; ++s) {
            int x = COL_X[1 + s];
            if (row.fx[s].cmd == 0) {
                d.text(x, y, "---", pal::FG_DIM);
            } else {
                char letter = (char)row.fx[s].cmd;
                if (letter < 0x20 || letter > 0x7E) letter = '?';
                d.glyph(x, y, letter, pal::FG_HEX);
                d.hex2(x + 7, y, row.fx[s].value, pal::FG);
            }
        }

        // cursor (breathing corner brackets, matching the phrase view)
        if (r == table_row_) {
            int slot = table_col_ / 2;
            bool is_cmd = (table_col_ & 1) == 0;
            int cx = COL_X[1 + slot] + (is_cmd ? -1 : 13);
            int cw = is_cmd ? 8 : 14;
            uint8_t ft = edit_flash_t();
            if (ft) d.rect(cx, y - 1, cw, 10, with_alpha(pal::FLASH, ft / 2));
            uint8_t br = breathe_pulse(frame_, 64);
            ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
            d.corner_brackets(cx, y - 1, cw, 10, cur, 3, 1);
        }
    }

    // === typed hint bar (same pattern as the phrase view): opaque backing so
    // the last rows can't bleed into the text ===
    {
        const int HY = 228;
        d.rect(0, HY - 2, 400, 14, pal::BG_HI);
        d.rect(0, HY - 3, 400, 1, pal::HEADER);
        int slot = table_col_ / 2;
        const auto& row = project_.tables[cur_table_].rows[table_row_];
        uint8_t cmd = row.fx[slot].cmd;
        char sl[5];
        std::snprintf(sl, sizeof(sl), "FX%d", slot + 1);
        d.text(8, HY, sl, pal::FG_DIM, 1);
        if (cmd == 0) {
            char hint[52];
            std::snprintf(hint, sizeof(hint),
                          "---  L+LR=TBL R+AB=SPD SEL=ASSIGN I%02X+PREV", cur_inst_);
            d.text(34, HY, hint, pal::FG_DIM, 1);
        } else {
            d.text(34, HY, seq::fx_name_short(cmd), pal::HEADER, 1);
            d.hex2(64, HY, row.fx[slot].value, pal::FG, 1);
            d.text(90, HY, seq::fx_name_long(cmd), pal::FG_DIM, 1);
            d.text(262, HY, "SEL=ASSIGN+PREV", pal::FG_DIM, 1);
        }
    }
}

} // namespace trackr::ui
