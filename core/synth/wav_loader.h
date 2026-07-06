// WAV loader: parses RIFF/WAVE -> q15 PCM -> resamples to 32kHz -> writes to Sample
//
// supported formats:
//   - 8-bit unsigned PCM (mono/stereo)
//   - 16-bit signed PCM (mono/stereo) - the main use case
//   - 24-bit signed PCM (mono/stereo)
//   - 32-bit float PCM (mono/stereo)
// ADPCM/MP3/OGG are not supported - convert via Audacity if needed
//
// resample: linear interpolation to target_sr (usually 32000)
// hard limit: max_frames (truncate if longer)
#pragma once
#include "sampler.h"

namespace trackr::synth {

enum class WavLoadResult : int8_t {
    Ok                 = 0,
    FileNotFound       = -1,
    NotRiff            = -2,
    NotWave            = -3,
    NoFmtChunk         = -4,
    NoDataChunk        = -5,
    UnsupportedFormat  = -6,    // ADPCM / unknown formatTag
    UnsupportedBits    = -7,    // 4-bit, 64-bit etc
    UnsupportedChannels = -8,   // >2 channels
    Truncated          = 1,     // ok but truncated to max_frames
};

const char* wav_result_str(WavLoadResult r);

// loads a WAV into a Sample (data + channels + root_note + chops/loops cleared)
// returns Ok or Truncated on success, negative on error
WavLoadResult load_wav_to_sample(const char* path, Sample& dst,
                                  int target_sr, int max_frames);

} // namespace trackr::synth
