// 3ds implementation of Draw via citro2d
// glyphs are drawn as a set of pixel-rects (slow but simple)
// for speed in the future - pre-render the font into a texture and draw with sprites
#pragma once
#include "../../core/ui/draw.h"
#include "../../core/ui/font.h"

namespace trackr::platform {

class Draw3DS : public ui::Draw {
public:
    void rect(int x, int y, int w, int h, ui::Color c) override;
    void glyph(int x, int y, char ch, ui::Color color, int scale) override;
};

} // namespace trackr::platform
