#pragma once
#include "runtime/audio.h"
#include <algorithm>
#include <vector>
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#endif

namespace sputnik::runtime {
struct DecodedAudioPCM { std::string bytes; std::uint32_t rate = 0, channels = 0; };
inline DecodedAudioPCM audio_system_decode(const std::string &bytes, const std::string &extension) {
#if defined(__APPLE__)
  // System frameworks are loaded lazily: headless/non-Apple workers can still
  // construct and transport encoded tracks without a codec or GUI dependency.
  static void *library = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LOCAL | RTLD_LAZY);
  if (!library) throw std::invalid_argument("system audio framework is unavailable");
  const auto open = reinterpret_cast<decltype(&AudioFileOpenWithCallbacks)>(dlsym(library,"AudioFileOpenWithCallbacks"));
  const auto close = reinterpret_cast<decltype(&AudioFileClose)>(dlsym(library,"AudioFileClose"));
  const auto wrap = reinterpret_cast<decltype(&ExtAudioFileWrapAudioFileID)>(dlsym(library,"ExtAudioFileWrapAudioFileID"));
  const auto dispose = reinterpret_cast<decltype(&ExtAudioFileDispose)>(dlsym(library,"ExtAudioFileDispose"));
  const auto get = reinterpret_cast<decltype(&ExtAudioFileGetProperty)>(dlsym(library,"ExtAudioFileGetProperty"));
  const auto set = reinterpret_cast<decltype(&ExtAudioFileSetProperty)>(dlsym(library,"ExtAudioFileSetProperty"));
  const auto read = reinterpret_cast<decltype(&ExtAudioFileRead)>(dlsym(library,"ExtAudioFileRead"));
  if (!open || !close || !wrap || !dispose || !get || !set || !read)
    throw std::invalid_argument("system audio decoder is unavailable");
  const auto format = audio_format(extension);
  AudioFileTypeID hint = 0;
  if (format == "wav" || format == "wave") hint = kAudioFileWAVEType;
  else if (format == "mp3") hint = kAudioFileMP3Type;
  else if (format == "aif" || format == "aiff") hint = kAudioFileAIFFType;
  else if (format == "m4a" || format == "mp4") hint = kAudioFileM4AType;
  else if (format == "aac") hint = kAudioFileAAC_ADTSType;
  else if (format == "caf") hint = kAudioFileCAFType;
  else if (format == "flac") hint = kAudioFileFLACType;
  else throw std::invalid_argument("unsupported system audio file extension: " + format);
  if (bytes.empty() || bytes.size() > kAudioMaxBytes) throw std::invalid_argument("invalid encoded audio size");
  AudioFileID file = nullptr; ExtAudioFileRef decoder = nullptr;
  struct Cleanup { AudioFileID &file; ExtAudioFileRef &decoder; decltype(close) close_fn; decltype(dispose) dispose_fn;
    ~Cleanup() { if (decoder) dispose_fn(decoder); if (file) close_fn(file); }
  } cleanup{file,decoder,close,dispose};
  auto check = [](OSStatus status) { if (status) throw std::invalid_argument("system audio decode failed (OSStatus " + std::to_string(status) + ")"); };
  auto reader = [](void *context, SInt64 position, UInt32 requested, void *out, UInt32 *count) -> OSStatus {
    const auto &input = *static_cast<const std::string *>(context); *count = 0;
    if (position < 0 || static_cast<std::uint64_t>(position) > input.size()) return kAudioFilePositionError;
    *count = static_cast<UInt32>(std::min<std::size_t>(requested, input.size() - position));
    std::memcpy(out, input.data() + position, *count); return noErr;
  };
  auto size = [](void *context) -> SInt64 { return static_cast<const std::string *>(context)->size(); };
  check(open(const_cast<std::string *>(&bytes), reader, nullptr, size, nullptr, hint, &file));
  check(wrap(file, false, &decoder));
  AudioStreamBasicDescription source{}; UInt32 property_size = sizeof(source);
  check(get(decoder, kExtAudioFileProperty_FileDataFormat, &property_size, &source));
  if (!std::isfinite(source.mSampleRate) || source.mSampleRate < 8000 || source.mSampleRate > 192000 ||
      source.mChannelsPerFrame < 1 || source.mChannelsPerFrame > 2 || source.mSampleRate != std::floor(source.mSampleRate))
    throw std::invalid_argument("system audio requires 8000..192000 Hz mono/stereo");
  AudioStreamBasicDescription target{};
  target.mSampleRate = source.mSampleRate; target.mChannelsPerFrame = source.mChannelsPerFrame;
  target.mFormatID = kAudioFormatLinearPCM; target.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  target.mBitsPerChannel = 32; target.mFramesPerPacket = 1; target.mBytesPerFrame = target.mBytesPerPacket = target.mChannelsPerFrame * 4;
  check(set(decoder,kExtAudioFileProperty_ClientDataFormat,sizeof(target),&target));
  DecodedAudioPCM result; result.rate = target.mSampleRate; result.channels = target.mChannelsPerFrame;
  std::vector<float> buffer(16384 * result.channels);
  while (true) {
    UInt32 frames = 16384;
    AudioBufferList list{}; list.mNumberBuffers = 1; list.mBuffers[0] = {result.channels,static_cast<UInt32>(buffer.size() * 4),buffer.data()};
    check(read(decoder,&frames,&list));
    if (!frames) break;
    const auto count = std::size_t(frames) * result.channels * 4;
    if (count > kAudioMaxBytes - 64 - result.bytes.size()) throw std::invalid_argument("decoded audio exceeds the track byte limit");
    result.bytes.append(reinterpret_cast<const char *>(buffer.data()),count);
  }
  (void)audio_pcm(result.bytes,"f32le",result.rate,result.channels,true);
  return result;
#else
  (void)bytes; (void)extension;
  throw std::invalid_argument("encoded audio decoding requires a system audio backend on this host");
#endif
}
inline std::string audio_convert_pcm(const std::string &bytes, const std::string &from,
    const std::string &to, std::uint32_t rate, std::uint32_t channels) {
  (void)audio_pcm(bytes,from,rate,channels,true);
  if (from == to) return bytes;
  if (to != "f32le" && to != "s16le") throw std::invalid_argument("unsupported PCM target format");
  const auto samples = bytes.size() / (from == "s16le" ? 2 : 4);
  if (samples > (kAudioMaxBytes - 64) / (to == "s16le" ? 2 : 4)) throw std::invalid_argument("converted PCM exceeds byte limit");
  std::string result; result.reserve(samples * (to == "s16le" ? 2 : 4));
  for (std::size_t i=0;i<samples;++i) {
    float value;
    if (from == "s16le") {
      const auto bits = std::uint16_t(static_cast<unsigned char>(bytes[i*2])) | (std::uint16_t(static_cast<unsigned char>(bytes[i*2+1])) << 8);
      value = static_cast<std::int16_t>(bits) / 32768.0f;
    } else { const auto bits = audio_read_u32(bytes,i*4); std::memcpy(&value,&bits,4); }
    if (to == "f32le") { std::uint32_t bits; std::memcpy(&bits,&value,4); audio_u32(result,bits); }
    else {
      if (value < -1 || value > 1) throw std::invalid_argument("PCM16 export requires samples in [-1,1]");
      const auto sample = static_cast<std::int16_t>(std::min(32767.0f,std::round(value * 32768.0f)));
      result.push_back(char(sample)); result.push_back(char(sample >> 8));
    }
  }
  return result;
}
}
