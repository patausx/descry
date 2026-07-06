#include "mic_3ds.h"
#include <malloc.h>
#include <cstring>

namespace trackr::platform {

bool Mic3DS::platform_init() {
    mic_buf_ = static_cast<uint8_t*>(memalign(0x1000, MIC_BUF_BYTES));
    if (!mic_buf_) return false;
    std::memset(mic_buf_, 0, MIC_BUF_BYTES);

    if (R_FAILED(micInit(mic_buf_, MIC_BUF_BYTES))) {
        free(mic_buf_);
        mic_buf_ = nullptr;
        return false;
    }
    sysmod_inited_ = true;
    return true;
}

void Mic3DS::platform_shutdown() {
    if (recording_) stop();
    if (sysmod_inited_) {
        micExit();
        sysmod_inited_ = false;
    }
    if (mic_buf_) {
        free(mic_buf_);
        mic_buf_ = nullptr;
    }
}

int Mic3DS::start(int /*requested_sr*/) {
    if (!sysmod_inited_) return 0;
    if (recording_) return 32728;

    // buffer size for StartSampling - take it from libctru
    // (this is the aligned size, you can't pass your own)
    sample_data_size_ = micGetSampleDataSize();

    // max sr - 32728, signed 16bit
    Result rc = MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED,
                                   MICU_SAMPLE_RATE_32730,
                                   0, sample_data_size_, true);
    if (R_FAILED(rc)) {
        return 0;
    }
    read_pos_ = micGetLastSampleOffset();
    recording_ = true;
    return 32728;
}

void Mic3DS::stop() {
    if (!recording_) return;
    MICU_StopSampling();
    recording_ = false;
}

std::size_t Mic3DS::poll(fx::q15* out, std::size_t max_frames) {
    if (!recording_) return 0;

    uint32_t write_pos = micGetLastSampleOffset();
    uint32_t buf_bytes = sample_data_size_;

    // compute how many bytes are available (accounting for the ring)
    uint32_t avail_bytes;
    if (write_pos >= read_pos_) {
        avail_bytes = write_pos - read_pos_;
    } else {
        avail_bytes = (buf_bytes - read_pos_) + write_pos;
    }

    // align to 16-bit samples (2 bytes)
    avail_bytes &= ~1u;
    std::size_t avail_frames = avail_bytes / 2;
    if (avail_frames > max_frames) avail_frames = max_frames;
    if (avail_frames == 0) return 0;

    // copy with ring unwrapping
    for (std::size_t i = 0; i < avail_frames; ++i) {
        int16_t s;
        std::memcpy(&s, mic_buf_ + read_pos_, 2);
        out[i] = static_cast<fx::q15>(s);
        read_pos_ = (read_pos_ + 2) % buf_bytes;
    }

    return avail_frames;
}

} // namespace trackr::platform
