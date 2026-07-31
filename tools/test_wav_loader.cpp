#include "core/synth/wav_loader.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace trackr;

static void u16(FILE* f, uint16_t v) { std::fputc(v & 255, f); std::fputc(v >> 8, f); }
static void u32(FILE* f, uint32_t v) { u16(f, v & 0xffff); u16(f, v >> 16); }

static void make_wav(const char* p, int sr, int ch, int frames, bool truncate_data = false) {
    FILE* f = std::fopen(p, "wb"); assert(f);
    const uint32_t bytes = frames * ch * 2;
    std::fwrite("RIFF",1,4,f); u32(f, 36 + bytes); std::fwrite("WAVE",1,4,f);
    std::fwrite("fmt ",1,4,f); u32(f,16); u16(f,1); u16(f,ch); u32(f,sr);
    u32(f,sr*ch*2); u16(f,ch*2); u16(f,16);
    std::fwrite("data",1,4,f); u32(f,bytes);
    int write_frames = truncate_data ? frames / 2 : frames;
    for (int i=0;i<write_frames*ch;i++) { int16_t s=(int16_t)((i*97)&0x7fff); u16(f,(uint16_t)s); }
    std::fclose(f);
}

int main() {
    make_wav("/tmp/descry_ok.wav", 44100, 2, 44100);
    synth::Sample s;
    auto r=synth::load_wav_to_sample("/tmp/descry_ok.wav",s,32000,32000*2);
    assert(r==synth::WavLoadResult::Ok); assert(s.channels==2); assert(s.num_frames()==32000);

    synth::Sample capped;
    r=synth::load_wav_to_sample("/tmp/descry_ok.wav",capped,32000,1000);
    assert(r==synth::WavLoadResult::Truncated); assert(capped.num_frames()==1000);

    make_wav("/tmp/descry_bad.wav", 44100, 1, 44100, true);
    synth::Sample bad; bad.data.assign(7,123);
    r=synth::load_wav_to_sample("/tmp/descry_bad.wav",bad,32000,32000);
    assert(r==synth::WavLoadResult::ReadError); assert(bad.data.size()==7 && bad.data[0]==123);

    FILE* f=std::fopen("/tmp/descry_tinyfmt.wav","wb"); assert(f);
    std::fwrite("RIFF",1,4,f);u32(f,14);std::fwrite("WAVEfmt ",1,8,f);u32(f,2);u16(f,1);std::fclose(f);
    synth::Sample tiny;
    r=synth::load_wav_to_sample("/tmp/descry_tinyfmt.wav",tiny,32000,1000);
    assert(r==synth::WavLoadResult::NoFmtChunk);
    std::puts("wav loader smoke: ok");
}
