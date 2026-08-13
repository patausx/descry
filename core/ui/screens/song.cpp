// App: song editor update, vertical arrangement and horizontal timeline.
#include "../app.h"
#include "../ui_internal.h"
#include "../../audio/fixed.h"
#include <cstdio>

namespace trackr::ui {

void App::update_song(const InputState& in) {
    const bool nav_up = in.up || in.analog_y > 0;
    const bool nav_down = in.down || in.analog_y < 0;
    const bool nav_left = in.left || in.analog_x < 0;
    const bool nav_right = in.right || in.analog_x > 0;
    // === ZL+SELECT = DEEP clone chain under cursor ===
    // copies the chain AND all phrases inside it into free slots, so the new
    // chain is fully independent (edit anything without touching the original).
    if (in.held_zl && in.select_) {
        auto& cell = project_.song.rows[song_row_].chain[song_col_];
        if (cell != seq::EMPTY) {
            int nc = project_.clone_chain_deep(cell);
            if (nc >= 0) {
                seq::EditRecord::Payload before, after;
                before.song_cell = cell;
                cell = (uint8_t)nc;
                after.song_cell = cell;
                undo_.record(seq::EditKind::SongCell, (uint16_t)song_row_, (uint16_t)song_col_,
                             before, after, frame_, 0);
                mark_project_dirty();
                edit_flash_frame_ = frame_;
                std::snprintf(clone_msg_, sizeof(clone_msg_), "CLONE %02X", nc);
            } else {
                std::snprintf(clone_msg_, sizeof(clone_msg_), "BANK FULL");
            }
            clone_msg_frame_ = frame_;
        }
        return;
    }

    // === ZL+LEFT/RIGHT = flip the song view orientation ===
    // vertical M8-style list <-> horizontal DAW-style timeline (DoubleSprattt).
    // same data, same cursor - just rotated axes.
    if (in.held_zl && (in.left || in.right)) {
        song_timeline_ = !song_timeline_;
        return;
    }

    // dpad nav: in timeline mode the axes rotate with the view -
    // left/right walk time (song rows), up/down hop tracks (lanes).
    if (song_timeline_) {
        if (nav_left)  song_row_ = wrap_index(song_row_, -1, seq::SONG_ROWS);
        if (nav_right) song_row_ = wrap_index(song_row_, +1, seq::SONG_ROWS);
        if (nav_up)    song_col_ = wrap_index(song_col_, -1, seq::NUM_TRACKS);
        if (nav_down)  song_col_ = wrap_index(song_col_, +1, seq::NUM_TRACKS);
    } else {
        if (nav_up)    song_row_ = wrap_index(song_row_, -1, seq::SONG_ROWS);
        if (nav_down)  song_row_ = wrap_index(song_row_, +1, seq::SONG_ROWS);
        if (nav_left)  song_col_ = wrap_index(song_col_, -1, seq::NUM_TRACKS);
        if (nav_right) song_col_ = wrap_index(song_col_, +1, seq::NUM_TRACKS);
    }

    // === live mode: Y = queue the chain under the cursor on its track (launches
    // on the next 16-step bar), X = queue a stop for the track. same press again
    // cancels. when nothing plays, Y starts immediately and sets the bar clock.
    if (in.y) {
        uint8_t c = project_.song.rows[song_row_].chain[song_col_];
        if (c != seq::EMPTY) player_.queue_chain(song_col_, c);
        return;
    }
    if (in.x && !in.held_zl) {
        if (player_.track_state(song_col_).playing)
            player_.queue_chain(song_col_, seq::Player::QUEUE_STOP);
        return;
    }

    auto& cell = project_.song.rows[song_row_].chain[song_col_];
    int delta = 0;
    if (in.a) delta = +1;
    if (in.b) delta = -1;
    if (in.encoder_delta) delta = in.encoder_delta;
    if (delta) {
        edit_flash_frame_ = frame_;   // cell-edit feedback, same as the phrase view
        const uint8_t before = cell;
        int v = (cell == seq::EMPTY) ? 0 : (cell + delta);
        if (v < 0) cell = seq::EMPTY;
        else if (v >= seq::MAX_CHAINS) cell = seq::MAX_CHAINS - 1;
        else cell = v;
        if (cell != before) mark_project_dirty();
    }
    if (in.select_ && cell != seq::EMPTY) {
        cur_chain_ = cell;
        screen_ = Screen::Chain;
    }
}
void App::draw_song(Draw& d) {
    if (song_timeline_) { draw_song_timeline(d); return; }

    constexpr int Y0 = 22;
    constexpr int ROW_H = 11;
    constexpr int VISIBLE = 18;

    // show a window around song_row
    int top_row = song_row_ - VISIBLE / 2;
    if (top_row < 0) top_row = 0;
    if (top_row > seq::SONG_ROWS - VISIBLE) top_row = seq::SONG_ROWS - VISIBLE;

    d.text(8, Y0, "##", pal::HEADER);
    // live-mode bar countdown: steps until the next quantized launch boundary.
    // shown only while something is queued (that's when you're counting).
    {
        bool any_q = false;
        for (int t = 0; t < seq::NUM_TRACKS; ++t)
            if (player_.queued(t) != seq::EMPTY) { any_q = true; break; }
        if (any_q && player_.playing()) {
            char qb[12];
            std::snprintf(qb, sizeof(qb), "BAR-%02d", player_.steps_to_bar());
            uint8_t br = breathe_pulse(frame_, 32);
            d.text(344, Y0, qb, lerp_color(pal::FG_DIM, pal::CURSOR, br));
        }
    }
    // per-track silence state, needed in three places below (header tag, column
    // wash, chain digits). muted = persisted project mask; solo-suppressed = the
    // momentary stage tool. both are INAUDIBLE, so both must be visible HERE -
    // the arrangement is what you stare at, the mixer is a place you visit.
    bool silent[seq::NUM_TRACKS];
    bool by_solo[seq::NUM_TRACKS];
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        bool m = (project_.track_mute & (1u << t)) != 0;
        bool s = (solo_track_ >= 0 && t != solo_track_);
        silent[t]  = m || s;
        by_solo[t] = s && !m;
    }

    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "T%d", t);
        d.text(40 + t * 42, Y0, buf, silent[t] ? pal::RECORD : pal::HEADER);
        // mute/solo tag on the free 6px at the right of the track column. M = muted,
        // S = the soloed track, s = silenced *because* someone else is soloed.
        // (queue badge lives at +14, cutoff bar above it - this slot stays clear.)
        if (silent[t]) {
            d.text(40 + t * 42 + 33, Y0 + 5, by_solo[t] ? "s" : "M",
                   by_solo[t] ? pal::FG_DIM : pal::RECORD);
        } else if (solo_track_ == t) {
            uint8_t br = breathe_pulse(frame_, 48);
            d.text(40 + t * 42 + 33, Y0 + 5, "S", lerp_color(pal::PLAY, pal::FG, br));
        }
        // mini cutoff/crush indicators alongside
        auto& tr = mixer_.track(t);
        // cutoff: horizontal bar 12px, filled by cutoff
        int cw = (tr.cutoff * 12) / fx::Q15_ONE;
        // cutoff bar color changes with filter_type - a visual indicator with no
        // extra pixels. THEMED (these used to be hardcoded cretaceous hexes, so on
        // vapor/ember the indicators stayed earthy-grey in a violet UI).
        uint32_t fc = pal::HEADER;
        switch (tr.filter_type) {
            case ::trackr::dsp::FilterType::LPF:   fc = pal::HEADER; break;  // label ochre
            case ::trackr::dsp::FilterType::HPF:   fc = pal::CURSOR; break;  // accent
            case ::trackr::dsp::FilterType::BPF:   fc = pal::FG_HEX; break;  // value tint
            case ::trackr::dsp::FilterType::Notch: fc = pal::TRACK3; break;  // 4th track hue
            case ::trackr::dsp::FilterType::Off:   fc = pal::GRID;   break;  // structure grey
        }
        d.rect(40 + t * 42 + 14, Y0 + 2, 12, 2, pal::GRID);
        d.rect(40 + t * 42 + 14, Y0 + 2, cw, 2, fc);
        // bits: height-bar on the right
        if (tr.bits < 16) {
            int bh = (16 - tr.bits) * 8 / 15;
            d.rect(40 + t * 42 + 27, Y0, 2, bh, pal::HEADER);
        }
        // live queue badge under the track header: queued chain id (or STP)
        uint8_t q = player_.queued(t);
        if (q != seq::EMPTY) {
            uint8_t br = breathe_pulse(frame_, 40);
            ui::Color qc = lerp_color(with_alpha(pal::CURSOR, 140), pal::CURSOR, br);
            if (q == seq::Player::QUEUE_STOP) d.text(40 + t * 42 + 14, Y0 + 5, "STP", qc, 1);
            else                              d.hex2(40 + t * 42 + 14, Y0 + 5, q, qc, 1);
            // thin honest countdown: full when queued, drains toward the launch bar.
            int rem = player_.steps_to_bar();
            if (rem == 0) rem = seq::Player::LIVE_QUANT_STEPS;
            int qw = rem * 28 / seq::Player::LIVE_QUANT_STEPS;
            d.rect(40 + t * 42 - 2, Y0 + 10, 28, 1, pal::GRID);
            d.rect(40 + t * 42 - 2, Y0 + 10, qw, 1, qc);
        }
        // launch/stop acknowledgement: one scanline crosses the track header.
        if (launch_motion_mask_ & (1u << t)) {
            uint32_t age = frame_ - launch_motion_frame_;
            if (age < 12) {
                int sx = 40 + t * 42 - 4 + (int)motion_out(age, 12) * 34 / 255;
                Color lc = (launch_stop_mask_ & (1u << t)) ? pal::RECORD : pal::FLASH;
                d.rect(sx, Y0 - 1, 2, 12, with_alpha(lc, (uint8_t)(220 - age * 14)));
            }
        }
    }

    // === song playhead trail bookkeeping: detect per-track row moves ===
    // (cached here because draw runs every frame and rows move rarely)
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        const auto& tps = player_.track_state(t);
        uint16_t cur = (tps.playing && tps.song_mode_) ? (uint16_t)tps.song_row : 0xFFFF;
        if (cur != song_ph_row_[t]) {
            song_ph_prev_[t]  = song_ph_row_[t];
            song_ph_row_[t]   = cur;
            song_ph_frame_[t] = frame_;
        }
    }

    // where the arrangement ends: the player loops at song_length(), so everything
    // below it is unreachable until you put a chain there. 256 identical empty rows
    // gave zero hint of that - you scroll into the void wondering if it still plays.
    const int song_len = player_.song_length();
    int cursor_y = -1;

    for (int i = 0; i < VISIBLE; ++i) {
        int row = top_row + i;
        int y = Y0 + 12 + i * ROW_H;
        const auto& sr = project_.song.rows[row];

        if (row & 0xF) {
            // normal
        } else {
            // every 16th highlighted
            d.rect(0, y - 1, 400, ROW_H, pal::BG_HI);
        }

        // dead zone past the end of the song (same language as the phrase view's
        // out-of-length shading, lighter: these rows are still editable and writing
        // a chain here extends the song).
        if (song_len > 0 && row >= song_len)
            d.rect(0, y - 1, 400, ROW_H, with_alpha(pal::PANEL, 0x70));

        // per-track playheads: each track advances through the song independently
        // (m8 model - chains of different lengths drift). so the playhead is a
        // CELL highlight per track, not a full-row line.
        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            const auto& tps = player_.track_state(t);
            int cx = 40 + t * 42;
            // ghost trail: the cell we just left fades out over ~24 frames
            if (song_ph_prev_[t] != 0xFFFF && (int)song_ph_prev_[t] == row) {
                uint32_t age = frame_ - song_ph_frame_[t];
                if (age < 24) {
                    uint8_t a = (uint8_t)(150 - age * 150 / 24);
                    d.rect(cx - 4, y - 1, 30, ROW_H, with_alpha(pal::PLAYHEAD_BG, a));
                }
            }
            if (!tps.playing || !tps.song_mode_ || (int)tps.song_row != row) continue;
            // cell backdrop + a small gradient flowing through it on each beat
            d.rect(cx - 4, y - 1, 30, ROW_H, with_alpha(pal::PLAYHEAD_BG, 0xC0));
            beat_glow(d, cx - 4, y - 1, 30, ROW_H, frame_ - step_change_frame_, pal::PLAYHEAD, 12);
            // left tick grows as the chain advances (row progress at a glance)
            int th = 2 + (int)tps.play_chain_row * (ROW_H - 2) / (seq::CHAIN_ROWS - 1);
            d.rect(cx - 4, y - 1, 2, th, pal::PLAYHEAD);
        }

        d.hex2(8, y, row & 0xFF, pal::FG_DIM);

        for (int t = 0; t < seq::NUM_TRACKS; ++t) {
            uint8_t c = sr.chain[t];
            // silenced tracks read dim even where they have content - otherwise a
            // muted column looks exactly as alive as a sounding one.
            Color live = silent[t] ? pal::FG_DIM : pal::FG;
            if (c == seq::EMPTY) d.text(40 + t * 42, y, "--", pal::FG_DIM);
            else d.hex2(40 + t * 42, y, c, live);
        }

        // end-of-song line, drawn ON TOP of the cells so it can't be buried
        if (song_len > 0 && row == song_len) {
            d.rect(0, y - 1, 400, 1, pal::CURSOR);
            d.text(346, y, "END", pal::CURSOR);
        }

        if (row == song_row_) cursor_y = y;   // drawn last, above the mute wash
    }

    // muted columns get a wash over the whole grid height: the fastest "why is this
    // silent" answer there is, and it survives scrolling. drawn OVER the playhead -
    // a muted track still advances, it just makes no sound, and that's the point.
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        if (!silent[t]) continue;
        d.rect(40 + t * 42 - 5, Y0 + 11, 32, VISIBLE * ROW_H,
               with_alpha(pal::PANEL, by_solo[t] ? 0x50 : 0x70));
    }

    // cursor last: it must stay legible even inside a washed-out muted column
    if (cursor_y >= 0) {
        int cx = 39 + song_col_ * 42;
        // value-flash on edit - the phrase view has had it forever; the song
        // cursor felt dead under the fingers without it.
        uint8_t ft = edit_flash_t();
        if (ft) d.rect(cx, cursor_y - 1, 14, 10, with_alpha(pal::FLASH, ft / 2));
        uint8_t br = breathe_pulse(frame_, 64);
        ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
        d.corner_brackets(cx, cursor_y - 1, 14, 10, cur, 3, 1);
    }
}

// === horizontal song timeline (M01D/DAW mental model) ===
// tracks are horizontal lanes, time flows left to right. same song data and
// cursor as the vertical list - ZL+LEFT/RIGHT flips between the two.
// per-track playheads drift independently (polymeter is VISIBLE here).
void App::draw_song_timeline(Draw& d) {
    constexpr int Y0      = 22;    // column-number header line
    constexpr int LX      = 26;    // lanes start x (track labels live left of it)
    constexpr int CELL_W  = 17;
    constexpr int COLS    = 22;    // 26 + 22*17 = 400
    constexpr int LANE_Y0 = 34;
    constexpr int LANE_H  = 23;    // 8 lanes -> 34..218

    // window of song rows around the cursor
    int left_col = song_row_ - COLS / 2;
    if (left_col < 0) left_col = 0;
    if (left_col > seq::SONG_ROWS - COLS) left_col = seq::SONG_ROWS - COLS;

    // === playhead trail bookkeeping (same cache as the vertical view) ===
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        const auto& tps = player_.track_state(t);
        uint16_t cur = (tps.playing && tps.song_mode_) ? (uint16_t)tps.song_row : 0xFFFF;
        if (cur != song_ph_row_[t]) {
            song_ph_prev_[t]  = song_ph_row_[t];
            song_ph_row_[t]   = cur;
            song_ph_frame_[t] = frame_;
        }
    }

    // column stripes (every 16th row) + header numbers (every 4th)
    for (int c = 0; c < COLS; ++c) {
        int row = left_col + c;
        int x = LX + c * CELL_W;
        if ((row & 0xF) == 0)
            d.rect(x - 2, LANE_Y0 - 2, CELL_W, seq::NUM_TRACKS * LANE_H + 2, pal::BG_HI);
        if ((row & 3) == 0)
            d.hex2(x, Y0, (uint8_t)(row & 0xFF), pal::FG_DIM);
    }
    d.text(2, Y0, "##", pal::HEADER);

    // end-of-song boundary: everything right of it never plays (the song loops at
    // song_length()). vertical line + tag, mirror of the list view's END row.
    const int song_len = player_.song_length();
    if (song_len > left_col && song_len < left_col + COLS) {
        int ex = LX + (song_len - left_col) * CELL_W - 2;
        d.rect(ex, LANE_Y0 - 2, 1, seq::NUM_TRACKS * LANE_H + 2, pal::CURSOR);
        d.text(ex + 2, Y0, "END", pal::CURSOR);
    }

    // same silence bookkeeping as the list view (mute mask + momentary solo)
    bool silent[seq::NUM_TRACKS], by_solo[seq::NUM_TRACKS];
    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        bool m = (project_.track_mute & (1u << t)) != 0;
        bool s = (solo_track_ >= 0 && t != solo_track_);
        silent[t]  = m || s;
        by_solo[t] = s && !m;
    }

    int cur_x = -1, cur_y = -1;

    for (int t = 0; t < seq::NUM_TRACKS; ++t) {
        int ly = LANE_Y0 + t * LANE_H;
        // lane separator
        d.rect(0, ly - 1, 400, 1, with_alpha(pal::GRID, 60));
        // track label + mute tag + live-queue badge
        char tb[4];
        std::snprintf(tb, sizeof(tb), "T%d", t);
        d.text(2, ly + 3, tb, silent[t] ? pal::RECORD
                                        : (t == song_col_ ? pal::CURSOR : pal::HEADER));
        if (silent[t]) {
            d.text(15, ly + 3, by_solo[t] ? "s" : "M",
                   by_solo[t] ? pal::FG_DIM : pal::RECORD);
        } else if (solo_track_ == t) {
            uint8_t br = breathe_pulse(frame_, 48);
            d.text(15, ly + 3, "S", lerp_color(pal::PLAY, pal::FG, br));
        }
        uint8_t q = player_.queued(t);
        if (q != seq::EMPTY) {
            uint8_t br = breathe_pulse(frame_, 40);
            ui::Color qc = lerp_color(with_alpha(pal::CURSOR, 140), pal::CURSOR, br);
            if (q == seq::Player::QUEUE_STOP) d.text(2, ly + 12, "STP", qc, 1);
            else                              d.hex2(2, ly + 12, q, qc, 1);
        }

        const auto& tps = player_.track_state(t);
        for (int c = 0; c < COLS; ++c) {
            int row = left_col + c;
            int x = LX + c * CELL_W;

            // past the end of the song: unreachable until a chain lands there
            if (song_len > 0 && row >= song_len)
                d.rect(x - 2, ly + 1, CELL_W, LANE_H - 2, with_alpha(pal::PANEL, 0x70));

            // ghost trail: the cell the playhead just left fades out
            if (song_ph_prev_[t] != 0xFFFF && (int)song_ph_prev_[t] == row) {
                uint32_t age = frame_ - song_ph_frame_[t];
                if (age < 24) {
                    uint8_t a = (uint8_t)(150 - age * 150 / 24);
                    d.rect(x - 2, ly + 1, CELL_W, LANE_H - 2, with_alpha(pal::PLAYHEAD_BG, a));
                }
            }
            // playhead cell + chain progress bar along the bottom edge
            if (tps.playing && tps.song_mode_ && (int)tps.song_row == row) {
                d.rect(x - 2, ly + 1, CELL_W, LANE_H - 2, with_alpha(pal::PLAYHEAD_BG, 0xC0));
                beat_glow(d, x - 2, ly + 1, CELL_W, LANE_H - 2,
                          frame_ - step_change_frame_, pal::PLAYHEAD, 12);
                int pw = 2 + (int)tps.play_chain_row * (CELL_W - 2) / (seq::CHAIN_ROWS - 1);
                d.rect(x - 2, ly + LANE_H - 3, pw, 2, pal::PLAYHEAD);
            }

            uint8_t ch = project_.song.rows[row].chain[t];
            if (ch == seq::EMPTY) d.text(x, ly + 7, "--", pal::FG_DIM);
            else                  d.hex2(x, ly + 7, ch, silent[t] ? pal::FG_DIM : pal::FG);

            if (row == song_row_ && t == song_col_) { cur_x = x; cur_y = ly; }
        }

        // muted lane wash across the full width, over the playhead
        if (silent[t])
            d.rect(LX - 2, ly + 1, COLS * CELL_W, LANE_H - 2,
                   with_alpha(pal::PANEL, by_solo[t] ? 0x50 : 0x70));
    }

    // cursor last so it stays readable inside a washed-out lane
    if (cur_x >= 0) {
        uint8_t ft = edit_flash_t();
        if (ft) d.rect(cur_x - 2, cur_y + 5, 16, 12, with_alpha(pal::FLASH, ft / 2));
        uint8_t br = breathe_pulse(frame_, 64);
        ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
        d.corner_brackets(cur_x - 2, cur_y + 5, 16, 12, cur, 3, 1);
    }

    // bottom strip: view hint + live-mode bar countdown
    d.text(2, 228, "ZL+< > LIST VIEW", pal::GRID);
    {
        bool any_q = false;
        for (int t = 0; t < seq::NUM_TRACKS; ++t)
            if (player_.queued(t) != seq::EMPTY) { any_q = true; break; }
        if (any_q && player_.playing()) {
            char qb[12];
            std::snprintf(qb, sizeof(qb), "BAR-%02d", player_.steps_to_bar());
            uint8_t br = breathe_pulse(frame_, 32);
            d.text(344, 228, qb, lerp_color(pal::FG_DIM, pal::CURSOR, br));
        }
    }
}

} // namespace trackr::ui
