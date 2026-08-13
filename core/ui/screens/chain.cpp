// App: chain editor update and drawing.
#include "../app.h"
#include "../ui_internal.h"
#include <cstdio>
#include <cstring>

namespace trackr::ui {

void App::update_chain(const InputState& in) {
    const bool nav_up = in.up;
    const bool nav_down = in.down;
    const bool nav_left = in.left;
    const bool nav_right = in.right;
    // === ZL+SELECT = clone phrase under cursor (lsdj-style "make unique") ===
    // copies the phrase into a free slot and points THIS row at the copy;
    // every other place that used the old phrase keeps playing it untouched.
    if (in.held_zl && in.select_) {
        auto& cell = project_.chains[cur_chain_].rows[chain_row_];
        if (cell.phrase != seq::EMPTY) {
            int np = project_.clone_phrase(cell.phrase);
            if (np >= 0) {
                seq::EditRecord::Payload before, after;
                before.chain = cell;
                cell.phrase = (uint8_t)np;
                after.chain = cell;
                undo_.record(seq::EditKind::ChainRow, cur_chain_, (uint16_t)chain_row_,
                             before, after, frame_, 0);
                mark_project_dirty();
                edit_flash_frame_ = frame_;
                std::snprintf(clone_msg_, sizeof(clone_msg_), "CLONE %02X", np);
            } else {
                std::snprintf(clone_msg_, sizeof(clone_msg_), "BANK FULL");
            }
            clone_msg_frame_ = frame_;
        }
        return;
    }

    if (nav_up)    chain_row_ = wrap_index(chain_row_, -1, seq::CHAIN_ROWS);
    if (nav_down)  chain_row_ = wrap_index(chain_row_, +1, seq::CHAIN_ROWS);
    if (nav_left)  chain_col_ = wrap_index(chain_col_, -1, 2);
    if (nav_right) chain_col_ = wrap_index(chain_col_, +1, 2);

    auto& cell = project_.chains[cur_chain_].rows[chain_row_];
    int delta = 0;
    if (in.a) delta = +1;
    if (in.b) delta = -1;
    if (in.encoder_delta) delta = in.encoder_delta;
    if (delta) {
        edit_flash_frame_ = frame_;
        const auto before = cell;
        if (chain_col_ == 0) {
            int v = (cell.phrase == seq::EMPTY) ? 0 : (cell.phrase + delta);
            if (v < 0) cell.phrase = seq::EMPTY;
            else if (v >= seq::MAX_PHRASES) cell.phrase = seq::MAX_PHRASES - 1;
            else cell.phrase = v;
        } else {
            bump_clamped(cell.transpose, delta, -64, 64);
        }
        if (std::memcmp(&before, &cell, sizeof(cell)) != 0) mark_project_dirty();
    }
    // open phrase under cursor
    if (in.select_ && cell.phrase != seq::EMPTY) {
        cur_phrase_ = cell.phrase;
        screen_ = Screen::Phrase;
    }
}
void App::draw_chain(Draw& d) {
    constexpr int Y0 = 22;
    constexpr int ROW_H = 13;

    d.text(40, Y0, "##", pal::HEADER);
    d.text(80, Y0, "PHR", pal::HEADER);
    d.text(140, Y0, "TSP", pal::HEADER);

    // which chain row is currently playing? same rule as the phrase view: prefer
    // the track the cursor is on, then the lowest one. play_chain_* is the row that
    // is SOUNDING (chain_row already points at the next row after the last step).
    int playing_row = -1;
    int playing_track = -1;
    if (player_.playing()) {
        auto match = [&](int t) {
            const auto& ts = player_.track_state(t);
            return ts.playing && ts.play_chain_id != seq::EMPTY &&
                   ts.play_chain_id == cur_chain_;
        };
        if (song_col_ >= 0 && song_col_ < seq::NUM_TRACKS && match(song_col_)) {
            playing_track = song_col_;
        } else {
            for (int t = 0; t < seq::NUM_TRACKS; ++t)
                if (match(t)) { playing_track = t; break; }
        }
        if (playing_track >= 0)
            playing_row = player_.track_state(playing_track).play_chain_row;
    }
    if (playing_track >= 0) {
        for (int i = 0; i < 4; ++i) d.rect(300 + i, Y0 + i, 1, 7 - i * 2, pal::PLAY);
        char tb[6];
        std::snprintf(tb, sizeof(tb), "T%d", playing_track);
        d.text(306, Y0, tb, pal::PLAY);
    }

    for (int row = 0; row < seq::CHAIN_ROWS; ++row) {
        int y = Y0 + 12 + row * ROW_H;
        const auto& r = project_.chains[cur_chain_].rows[row];

        if (row & 1) d.rect(0, y - 1, 400, ROW_H, pal::BG_HI);

        // playhead: highlight the row currently playing this chain, with the same
        // flowing-gradient feedback as the phrase view.
        if (row == playing_row) {
            d.rect(0, y - 1, 400, ROW_H, with_alpha(pal::PLAY, 0x60));
            beat_glow(d, 0, y - 1, 400, ROW_H, frame_ - step_change_frame_, pal::PLAY);
            d.rect(0, y - 1, 3, ROW_H, pal::PLAY);
            d.rect(397, y - 1, 3, ROW_H, pal::PLAY);
        }

        d.hex2(40, y, row, pal::FG_DIM);
        if (r.phrase == seq::EMPTY) d.text(80, y, "--", pal::FG_DIM);
        else d.hex2(80, y, r.phrase, pal::FG);

        // transpose as signed
        if (r.transpose == 0) d.text(140, y, "00", pal::FG_DIM);
        else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%+03d", r.transpose);
            d.text(140, y, buf, pal::FG);
        }

        if (row == chain_row_) {
            int cx = (chain_col_ == 0) ? 79 : 139;
            uint8_t br = breathe_pulse(frame_, 64);
            ui::Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
            d.corner_brackets(cx, y - 1, 14, 10, cur, 3, 1);
        }
    }
}

} // namespace trackr::ui
