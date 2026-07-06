#include "wav_loader.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

namespace trackr::synth {

// === RIFF helpers ===

static uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t read_u16_le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// reads one chunk header (id 4b + size 4b)
static bool read_chunk_header(FILE* f, char id[5], uint32_t& size) {
    uint8_t buf[8];
    if (std::fread(buf, 1, 8, f) != 8) return false;
    std::memcpy(id, buf, 4);
    id[4] = 0;
    size = read_u32_le(buf + 4);
    return true;
}

// === sample → q15 conversion ===

// reads one PCM sample from buf by format/bits, returns q15
// pos - the sample index (channels already accounted for)
static int16_t pcm_to_q15(const uint8_t* buf, std::size_t pos,
                          uint16_t format, uint16_t bits) {
    if (format == 1) {     // PCM int
        if (bits == 8) {
            // unsigned 8-bit, center = 128
            int s = (int)buf[pos] - 128;
            return (int16_t)(s << 8);     // [-128..127] → [-32768..32512]
        } else if (bits == 16) {
            return (int16_t)read_u16_le(buf + pos * 2);
        } else if (bits == 24) {
            const uint8_t* p = buf + pos * 3;
            int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
            // sign-extend 24→32
            if (v & 0x800000) v |= 0xFF000000;
            return (int16_t)(v >> 8);     // 24→16
        } else if (bits == 32) {
            int32_t v = (int32_t)read_u32_le(buf + pos * 4);
            return (int16_t)(v >> 16);
        }
    } else if (format == 3 && bits == 32) {     // IEEE float
        uint32_t u = read_u32_le(buf + pos * 4);
        float f;
        std::memcpy(&f, &u, sizeof(f));
        if (f >  1.0f) f =  1.0f;
        if (f < -1.0f) f = -1.0f;
        return (int16_t)(f * 32767.0f);
    }
    return 0;
}

const char* wav_result_str(WavLoadResult r) {
    switch (r) {
        case WavLoadResult::Ok:                  return "OK";
        case WavLoadResult::Truncated:           return "TRUNCATED";
        case WavLoadResult::FileNotFound:        return "NOT FOUND";
        case WavLoadResult::NotRiff:             return "NOT RIFF";
        case WavLoadResult::NotWave:             return "NOT WAVE";
        case WavLoadResult::NoFmtChunk:          return "NO FMT";
        case WavLoadResult::NoDataChunk:         return "NO DATA";
        case WavLoadResult::UnsupportedFormat:   return "BAD FORMAT";
        case WavLoadResult::UnsupportedBits:     return "BAD BITS";
        case WavLoadResult::UnsupportedChannels: return "BAD CHANS";
    }
    return "?";
}

WavLoadResult load_wav_to_sample(const char* path, Sample& dst,
                                  int target_sr, int max_frames) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return WavLoadResult::FileNotFound;

    // === RIFF header ===
    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12) { std::fclose(f); return WavLoadResult::NotRiff; }
    if (std::memcmp(hdr, "RIFF", 4) != 0) { std::fclose(f); return WavLoadResult::NotRiff; }
    if (std::memcmp(hdr + 8, "WAVE", 4) != 0) { std::fclose(f); return WavLoadResult::NotWave; }

    // === scan chunks: look for 'fmt ' and 'data' ===
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    long data_offset = -1;
    uint32_t data_size = 0;

    char id[5];
    uint32_t csize;
    while (read_chunk_header(f, id, csize)) {
        if (std::memcmp(id, "fmt ", 4) == 0) {
            // fmt chunk: min. 16 bytes
            uint8_t fmt_buf[40];
            uint32_t to_read = csize > 40 ? 40 : csize;
            if (std::fread(fmt_buf, 1, to_read, f) != to_read) break;
            format       = read_u16_le(fmt_buf + 0);
            channels     = read_u16_le(fmt_buf + 2);
            sample_rate  = read_u32_le(fmt_buf + 4);
            // byte_rate at +8, block_align at +12
            bits         = read_u16_le(fmt_buf + 14);
            // skip the rest of the chunk (in case there was WAVEFORMATEX extra)
            if (csize > to_read) std::fseek(f, csize - to_read, SEEK_CUR);
            // align to an even byte (RIFF padding)
            if (csize & 1) std::fseek(f, 1, SEEK_CUR);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_offset = std::ftell(f);
            data_size   = csize;
            // don't read the body now - after fully decoding fmt
            std::fseek(f, csize, SEEK_CUR);
            if (csize & 1) std::fseek(f, 1, SEEK_CUR);
        } else {
            // skip unknown chunk
            std::fseek(f, csize, SEEK_CUR);
            if (csize & 1) std::fseek(f, 1, SEEK_CUR);
        }
    }

    if (format == 0 || sample_rate == 0) { std::fclose(f); return WavLoadResult::NoFmtChunk; }
    if (data_offset < 0)                  { std::fclose(f); return WavLoadResult::NoDataChunk; }
    if (channels < 1 || channels > 2)     { std::fclose(f); return WavLoadResult::UnsupportedChannels; }

    // format/bits check
    bool fmt_ok = false;
    if (format == 1) {
        if (bits == 8 || bits == 16 || bits == 24 || bits == 32) fmt_ok = true;
    } else if (format == 3 && bits == 32) {
        fmt_ok = true;
    }
    if (!fmt_ok) {
        std::fclose(f);
        return (format == 1) ? WavLoadResult::UnsupportedBits : WavLoadResult::UnsupportedFormat;
    }

    // === read the data chunk ===
    int bytes_per_sample = bits / 8;
    int frame_bytes = bytes_per_sample * channels;
    if (frame_bytes <= 0) { std::fclose(f); return WavLoadResult::UnsupportedFormat; }

    uint32_t total_src_frames = data_size / frame_bytes;
    if (total_src_frames == 0) { std::fclose(f); return WavLoadResult::NoDataChunk; }

    // === resample ratio ===
    // src_pos advances by ratio per output frame
    // ratio = src_sr / target_sr (in floats, q24.8 for speed)
    double ratio = (double)sample_rate / (double)target_sr;
    // expected number of output frames
    uint32_t out_frames_expected = (uint32_t)((double)total_src_frames / ratio);
    bool truncated = false;
    if ((int)out_frames_expected > max_frames) {
        out_frames_expected = (uint32_t)max_frames;
        truncated = true;
    }

    // === read everything into RAM at once (compact) ===
    std::vector<uint8_t> raw(data_size);
    std::fseek(f, data_offset, SEEK_SET);
    if (std::fread(raw.data(), 1, data_size, f) != data_size) {
        std::fclose(f);
        return WavLoadResult::NoDataChunk;
    }
    std::fclose(f);

    // === decode + resample into q15 ===
    dst.data.assign((std::size_t)out_frames_expected * channels, 0);

    double src_pos = 0.0;
    for (uint32_t i = 0; i < out_frames_expected; ++i) {
        uint32_t src_idx = (uint32_t)src_pos;
        double frac = src_pos - (double)src_idx;
        if (src_idx + 1 >= total_src_frames) {
            // hit end of source - leave the rest as zeros
            break;
        }
        for (int ch = 0; ch < channels; ++ch) {
            std::size_t pos_a = (std::size_t)src_idx * channels + ch;
            std::size_t pos_b = pos_a + channels;
            int16_t a = pcm_to_q15(raw.data(), pos_a, format, bits);
            int16_t b = pcm_to_q15(raw.data(), pos_b, format, bits);
            int32_t lerp = a + (int32_t)((b - a) * frac);
            if (lerp >  32767) lerp =  32767;
            if (lerp < -32768) lerp = -32768;
            dst.data[(std::size_t)i * channels + ch] = (fx::q15)lerp;
        }
        src_pos += ratio;
    }

    // === fill in the Sample's metadata ===
    dst.channels   = (uint8_t)channels;
    dst.root_note  = 60;
    dst.loop_start = 0;
    dst.loop_end   = 0;
    dst.reversed   = false;
    for (int i = 0; i < Sample::MAX_CHOPS; ++i) dst.chops[i] = 0xFFFFFFFFu;

    return truncated ? WavLoadResult::Truncated : WavLoadResult::Ok;
}

} // namespace trackr::synth
