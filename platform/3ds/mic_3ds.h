// 3ds microphone via libctru MIC service
// hw supports 8180/16360/32728 hz signed/unsigned 8/16-bit
// we want 32728 hz signed 16-bit, then resample in core to 32000 if needed
// (the 2.3% difference is nearly inaudible, can just read it as 32000)
#pragma once
#include "../../core/synth/mic_recorder.h"
#include <3ds.h>
#include <cstdint>

namespace trackr::platform {

class Mic3DS : public synth::MicRecorder {
public:
    bool platform_init();
    void platform_shutdown();

    int  start(int requested_sr) override;
    void stop() override;
    std::size_t poll(fx::q15* out, std::size_t max_frames) override;
    bool is_recording() const override { return recording_; }

private:
    static constexpr std::size_t MIC_BUF_BYTES = 0x30000;  // 192kb circular
    uint8_t* mic_buf_ = nullptr;
    uint32_t sample_data_size_ = 0;     // returns micGetSampleDataSize() after init
    uint32_t read_pos_ = 0;
    bool recording_ = false;
    bool sysmod_inited_ = false;
};

} // namespace trackr::platform
