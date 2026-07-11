// App: project menu screen (slot load/save/new/delete + rename). Split out of app.cpp.
// Owns update_project + draw_project.
// v2: slot cells show the saved project's NAME (peeked by main into slot_names),
// current-project banner on top, breathing cursor, fading status toast.
// v3: R-held rename mode (A/B cycle char, L/R move cursor, X clear).
#include "../app.h"
#include "../ui_internal.h"
#include <cstdio>
#include <cstring>

namespace trackr::ui {

// character set for project rename (space first = default pad)
static constexpr const char* RENAME_CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";
static constexpr int RENAME_SET_LEN = 39; // strlen of above

static int char_index(char c) {
    for (int i = 0; i < RENAME_SET_LEN; ++i)
        if (RENAME_CHARS[i] == c) return i;
    return 0; // unknown -> space
}

void App::update_project(const InputState& in) {
    constexpr int COLS = 4;
    constexpr int N = 16;
    // editable positions 0..22: name[23] stays '\0' FOREVER so every %s-style
    // consumer (slot peek, header banner) always sees a terminated string
    constexpr int NAME_MAX = 23;

    // === R-held: rename mode ===
    if (in.held_r) {
        if (in.left)  { rename_pos_ = (rename_pos_ - 1 + NAME_MAX) % NAME_MAX; }
        if (in.right) { rename_pos_ = (rename_pos_ + 1) % NAME_MAX; }

        if (in.a) {
            // cycle character forward. positions before the cursor that are
            // still '\0' become spaces so the string stays contiguous.
            for (int i = 0; i < rename_pos_; ++i)
                if (project_.name[i] == 0) project_.name[i] = ' ';
            int ci = char_index(project_.name[rename_pos_]);
            ci = (ci + 1) % RENAME_SET_LEN;
            project_.name[rename_pos_] = RENAME_CHARS[ci];
            project_.name[NAME_MAX] = 0;   // terminator is sacred
            dirty = true;
        }
        if (in.b) {
            // cycle character backward
            for (int i = 0; i < rename_pos_; ++i)
                if (project_.name[i] == 0) project_.name[i] = ' ';
            int ci = char_index(project_.name[rename_pos_]);
            ci = (ci - 1 + RENAME_SET_LEN) % RENAME_SET_LEN;
            project_.name[rename_pos_] = RENAME_CHARS[ci];
            project_.name[NAME_MAX] = 0;
            dirty = true;
        }
        if (in.x) {
            // clear character to space
            project_.name[rename_pos_] = ' ';
            project_.name[NAME_MAX] = 0;
            dirty = true;
        }
        return; // don't process grid navigation while renaming
    }

    // === normal mode: grid navigation ===
    bool moved = false;
    if (in.up)    { proj_slot_ = (proj_slot_ - COLS + N) % N; moved = true; }
    if (in.down)  { proj_slot_ = (proj_slot_ + COLS) % N;     moved = true; }
    if (in.left)  { proj_slot_ = (proj_slot_ - 1 + N) % N;    moved = true; }
    if (in.right) { proj_slot_ = (proj_slot_ + 1) % N;        moved = true; }

    // any movement or other action - reset pending delete / pending load
    if (moved || in.x || in.y) { proj_confirm_delete_ = -1; proj_confirm_load_ = -1; }
    if (in.a) proj_confirm_delete_ = -1;
    if (in.b) proj_confirm_load_   = -1;

    if (in.a) {
        // loading over unsaved work nukes it silently - that cost a discord user
        // a whole track. dirty project -> ask for a second press (delete-style).
        if (dirty && proj_confirm_load_ != proj_slot_) {
            proj_confirm_load_ = proj_slot_;
            std::snprintf(slot_status, sizeof(slot_status),
                "UNSAVED CHANGES - PRESS A AGAIN TO LOAD");
            slot_status_frame_ = frame_;
        } else {
            proj_confirm_load_ = -1;
            proj_action_slot = proj_slot_; proj_action = ProjAction::Load;
        }
    }
    if (in.y) { proj_action_slot = proj_slot_; proj_action = ProjAction::Save; }
    if (in.x) { proj_action_slot = proj_slot_; proj_action = ProjAction::New; }
    if (in.b) {
        if (proj_confirm_delete_ == proj_slot_) {
            // second press on the same slot - confirmed
            proj_action_slot = proj_slot_;
            proj_action = ProjAction::Delete;
            proj_confirm_delete_ = -1;
        } else if (slot_present[proj_slot_]) {
            // first press on a saved slot - wait for confirmation
            proj_confirm_delete_ = proj_slot_;
            std::snprintf(slot_status, sizeof(slot_status),
                "PRESS B AGAIN TO DELETE SLOT %02X", proj_slot_);
            slot_status_frame_ = frame_;
        }
        // empty slot - do nothing
    }
}

void App::draw_project(Draw& d) {
    constexpr int COLS = 4;
    constexpr int CELL_W = 94;
    constexpr int CELL_H = 40;
    constexpr int X0 = 10;
    constexpr int Y0 = 44;

    char buf[48];

    // === header: title + current project banner ===
    d.text(10, 14, "PROJECT", pal::HEADER, 1);
    d.text(66, 14, "A=LOAD Y=SAVE X=NEW B=DEL(2x)", pal::FG_DIM);

    // current project name line — doubles as rename target when R held
    {
        // build display string: "NOW: <name>" with dirty marker
        char name_buf[28];
        // ensure name is null-terminated for display
        std::snprintf(name_buf, sizeof(name_buf), "%.24s", project_.name);
        // trim trailing spaces for display (but keep them in the actual buffer)

        std::snprintf(buf, sizeof(buf), "NOW: %s%s", name_buf, dirty ? " *" : "");
        d.text(10, 28, buf, dirty ? pal::CURSOR : pal::FG);

        // rename cursor: blinking underline under active character when R held
        if (mod_r_) {
            // show R-mode hint instead of the save hint
            d.text(250, 28, "R:RENAME", pal::PLAY);

            // underline position: "NOW: " is 5 chars × 6px each = 30px offset
            int cur_x = 10 + 5 * 6 + rename_pos_ * 6;
            uint8_t blink = breathe_pulse(frame_, 20);
            Color ul_c = lerp_color(pal::CURSOR, pal::PLAY, blink);
            d.rect(cur_x, 36, 5, 1, ul_c);
            // also show a small caret above
            d.rect(cur_x + 2, 26, 1, 2, ul_c);
        } else {
            d.text(280, 28, "*=unsaved", pal::FG_DIM);
        }
    }

    for (int i = 0; i < 16; ++i) {
        int col = i % COLS;
        int row = i / COLS;
        int x = X0 + col * CELL_W;
        int y = Y0 + row * CELL_H;

        bool selected = (i == proj_slot_);
        bool present  = slot_present[i];
        bool confirm  = (i == proj_confirm_delete_);
        bool confirm_load = (i == proj_confirm_load_);
        Color bg, border;
        if (confirm) {
            // pending delete: pulse the terracotta so it feels alarmed
            uint8_t p = breathe_pulse(frame_, 24);
            bg = lerp_color(0xFF5C3434, 0xFF7C4545, p);
            border = pal::RECORD;
        } else if (confirm_load) {
            // pending load-over-unsaved: amber pulse - caution, not alarm
            uint8_t p = breathe_pulse(frame_, 24);
            bg = lerp_color(0xFF5C4A28, 0xFF7C6335, p);
            border = pal::CURSOR;
        } else if (selected) {
            bg = present ? 0xFF675239 : pal::GRID;
            border = pal::CURSOR;
        } else if (present) {
            bg = pal::BG_HI;
            border = pal::HEADER;
        } else {
            bg = pal::BG;
            border = pal::GRID;
        }
        ui_button(d, x, y, CELL_W - 4, CELL_H - 4, bg, border);
        // selected cell gets breathing corner brackets (matches every other view)
        if (selected && !confirm && !confirm_load) {
            uint8_t br = breathe_pulse(frame_, 64);
            Color cur = lerp_color(with_alpha(pal::CURSOR, 130), pal::CURSOR, br);
            d.corner_brackets(x - 2, y - 2, CELL_W, CELL_H, cur, 4, 1);
        }

        // slot number (small, top-left) + content line
        std::snprintf(buf, sizeof(buf), "%02X", i);
        d.text(x + 6, y + 6, buf, present ? pal::HEADER : pal::FG_DIM, 1);

        if (confirm) {
            d.text(x + 26, y + 6, "DEL?", pal::FG);
            d.text(x + 6, y + 20, "B=yes move=no", pal::FG_DIM);
        } else if (confirm_load) {
            d.text(x + 26, y + 6, "LOAD?", pal::FG);
            d.text(x + 6, y + 20, "A=yes move=no", pal::FG_DIM);
        } else if (present) {
            // saved project's actual name (truncate to fit ~13 glyphs)
            char nm[15];
            std::snprintf(nm, sizeof(nm), "%.13s",
                          slot_names[i][0] ? slot_names[i] : "???");
            d.text(x + 6, y + 20, nm, pal::FG);
        } else {
            d.text(x + 6, y + 20, selected ? "A = blank" : "-", 
                   selected ? pal::PLAY : pal::FG_DIM);
        }
    }

    // === status toast: bright for ~2s after the event, then dims ===
    if (slot_status[0]) {
        // detect text change (main writes the buffer directly) -> restart the fade
        static uint32_t last_hash = 0;
        uint32_t h = 5381;
        for (const char* p = slot_status; *p; ++p) h = h * 33 + (uint8_t)*p;
        if (h != last_hash) { last_hash = h; slot_status_frame_ = frame_; }
        uint32_t age = frame_ - slot_status_frame_;
        Color c = (age < 120) ? pal::PLAY : pal::FG_DIM;
        d.text(10, 224, slot_status, c);
    } else {
        d.text(10, 224, "session = autosave on exit", pal::FG_DIM);
    }
}

} // namespace trackr::ui
