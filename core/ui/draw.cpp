#include "draw.h"
#include "../sequencer/types.h"

namespace trackr::ui {

void Draw::note_str(uint8_t note, char out[4]) {
    if (note == seq::EMPTY || note == 0xFF) {
        out[0] = '-'; out[1] = '-'; out[2] = '-'; out[3] = 0;
        return;
    }
    static const char* names[12] = {
        "C-", "C#", "D-", "D#", "E-", "F-",
        "F#", "G-", "G#", "A-", "A#", "B-"
    };
    int oct = note / 12 - 1;
    int n   = note % 12;
    out[0] = names[n][0];
    out[1] = names[n][1];
    if (oct < 0) oct = 0;
    if (oct > 9) oct = 9;
    out[2] = '0' + oct;
    out[3] = 0;
}

void Draw::note(int x, int y, uint8_t n, Color color, int scale) {
    char buf[4];
    note_str(n, buf);
    text(x, y, buf, color, scale);
}

} // namespace trackr::ui
