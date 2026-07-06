// FX commands — m8-style
// each command = char (cmd) + uint8 (value)
// executed on step trigger, some stay active for the whole step
#pragma once
#include <cstdint>

namespace trackr::seq {

// reserved: 0 = none/empty
namespace fx_cmd {
    constexpr uint8_t NONE = 0x00;

    // === note-level (applied when triggering a note) ===
    constexpr uint8_t VOL  = 'V';   // 'V' xx → volume 00..FF (0..Q15_ONE)
    constexpr uint8_t PIT  = 'P';   // 'P' xx -> pitch shift in semitones (signed: 80=0, 81=+1, 7F=-1)
    constexpr uint8_t KIL  = 'K';   // 'K' xx -> kill note after xx ticks
    constexpr uint8_t RTG  = 'R';   // 'R' xx -> retrigger the note xx times within the step
    constexpr uint8_t OFF  = 'X';   // 'X' xx -> note-off (release): 00=now, xx=after xx ticks
    constexpr uint8_t ARP  = 'J';   // 'J' xy -> arpeggio: cycle base, +x, +y semitones (nibbles) across the step
    constexpr uint8_t DLY  = 'D';   // 'D' xx -> delay this step's note_on by xx ticks (flam/ghost)
    constexpr uint8_t FIL  = 'F';   // 'F' xx → filter cutoff (00=closed, FF=open)
    constexpr uint8_t CRU  = 'B';   // 'B' xx → bitcrush bits (00=clean=16, FF=most crush)
    constexpr uint8_t SND  = 'S';   // 'S' xx -> send level to BOTH delay+reverb (00=dry, FF=full)
    constexpr uint8_t SDL  = 'E';   // 'E' xx -> send to DELAY only (m8 DEL-style)
    constexpr uint8_t SRV  = 'G';   // 'G' xx -> send to REVERB only (m8 REV-style)
    constexpr uint8_t RES  = 'Q';   // 'Q' xx → filter resonance (00=clean, FF=self-osc)
    constexpr uint8_t FTY  = 'Y';   // 'Y' xx -> filter type: 0=LPF 1=HPF 2=BPF 3=Notch 4=Off (others = LPF)
    constexpr uint8_t CHA  = 'O';   // 'O' xx -> trigger chance: probability the step fires (00=~never, 80=50%, FF=always)
    constexpr uint8_t EVN  = 'C';   // 'C' xy -> condition: play on pass x of every y phrase loops (1-based).
                                    //          14 = 1st of 4, 34 = 3rd of 4. y=0 -> always. (polyend-style)
    constexpr uint8_t PAN  = 'A';   // 'A' xx -> stereo pan (signed: 00=hard L, 80=center, FF=hard R)

    // === MG (LFO) - per-track modulation ===
    constexpr uint8_t LFR  = 'L';   // 'L' xx → MG rate (00=slow ~0.1Hz, FF=fast ~20Hz)
    constexpr uint8_t MGC  = 'M';   // 'M' xx → MG → cutoff depth (signed: 80=0, FF=+max, 00=-max)
    constexpr uint8_t MGV  = 'N';   // 'N' xx → MG → VCA/amplitude depth (signed: 80=0, FF=+max, 00=-max)
    constexpr uint8_t MGW  = 'W';   // 'W' xx → MG waveform: 0=TRI 1=SAW 2=SQR 3=S&H

    // === song-level ===
    constexpr uint8_t TMP  = 'T';   // 'T' xx → set BPM
    constexpr uint8_t HOP  = 'H';   // 'H' xx -> jump to step xx in this phrase
}

// convenient helper: iterate over the 3 fx slots of a step and apply them
// (called from Player::trigger_step)

// max editable value for an FX command. enum-like commands stop at their last
// option so the value can't scroll into a meaningless range (what you see = what
// you hear). most continuous params go to FF.
inline uint8_t fx_value_max(uint8_t cmd) {
    switch (cmd) {
        case fx_cmd::FTY: return 4;    // LPF/HPF/BPF/NTC/OFF
        case fx_cmd::MGW: return 3;    // TRI/SAW/SQR/S&H
        case fx_cmd::KIL: return 6;    // ticks: up to one full step (TICKS_PER_STEP)
        case fx_cmd::OFF: return 6;    // same
        case fx_cmd::DLY: return 5;    // must stay below one step (TICKS_PER_STEP-1)
        case fx_cmd::RTG: return 6;    // retrig tick-period, capped at one step
        case fx_cmd::HOP: return 15;   // step index 0..F
        default:          return 255;  // VOL/PAN/PIT/CUT/RES/SND/CRU/ARP/LFR/MGC/MGV/TMP/CHA/EVN
    }
}
// short() = 3-letter mnemonic for compact display; long() = full word for the hint bar.
inline const char* fx_name_short(uint8_t cmd) {
    switch (cmd) {
        case fx_cmd::VOL: return "VOL";
        case fx_cmd::PIT: return "PIT";
        case fx_cmd::KIL: return "KIL";
        case fx_cmd::RTG: return "RTG";
        case fx_cmd::OFF: return "OFF";
        case fx_cmd::ARP: return "ARP";
        case fx_cmd::DLY: return "DLY";
        case fx_cmd::FIL: return "CUT";
        case fx_cmd::CRU: return "CRU";
        case fx_cmd::SND: return "SND";
        case fx_cmd::SDL: return "DEL";
        case fx_cmd::SRV: return "REV";
        case fx_cmd::RES: return "RES";
        case fx_cmd::FTY: return "FTY";
        case fx_cmd::CHA: return "CHA";
        case fx_cmd::EVN: return "EVN";
        case fx_cmd::PAN: return "PAN";
        case fx_cmd::LFR: return "LFO";
        case fx_cmd::MGC: return "M>C";
        case fx_cmd::MGV: return "M>V";
        case fx_cmd::MGW: return "MGW";
        case fx_cmd::TMP: return "BPM";
        case fx_cmd::HOP: return "HOP";
        default: return "---";
    }
}

inline const char* fx_name_long(uint8_t cmd) {
    switch (cmd) {
        case fx_cmd::VOL: return "VOLUME";
        case fx_cmd::PIT: return "PITCH (semitones)";
        case fx_cmd::KIL: return "KILL after ticks";
        case fx_cmd::RTG: return "RETRIGGER xN";
        case fx_cmd::OFF: return "NOTE-OFF (release)";
        case fx_cmd::ARP: return "ARPEGGIO (nibbles)";
        case fx_cmd::DLY: return "NOTE DELAY (ticks)";
        case fx_cmd::FIL: return "FILTER CUTOFF";
        case fx_cmd::CRU: return "BITCRUSH";
        case fx_cmd::SND: return "SEND to delay+reverb";
        case fx_cmd::SDL: return "SEND to delay only";
        case fx_cmd::SRV: return "SEND to reverb only";
        case fx_cmd::RES: return "FILTER RESONANCE";
        case fx_cmd::FTY: return "FILTER TYPE LP/HP/BP/N/off";
        case fx_cmd::CHA: return "CHANCE (trigger prob.)";
        case fx_cmd::EVN: return "EVERY (pass x of y loops)";
        case fx_cmd::PAN: return "PAN (L<->R)";
        case fx_cmd::LFR: return "LFO RATE";
        case fx_cmd::MGC: return "LFO->CUTOFF depth";
        case fx_cmd::MGV: return "LFO->VOLUME depth";
        case fx_cmd::MGW: return "LFO WAVE TRI/SAW/SQR/S&H";
        case fx_cmd::TMP: return "TEMPO (BPM)";
        case fx_cmd::HOP: return "HOP to step";
        default: return "";
    }
}

} // namespace trackr::seq
