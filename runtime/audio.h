#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <map>
#include <memory>

namespace sputnik::runtime {
inline constexpr std::size_t kAudioMaxBytes = 16U * 1024U * 1024U;
inline constexpr const char *kAudioMime = "application/vnd.sputnik.audio";
inline void audio_u32(std::string &s, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) s.push_back(static_cast<char>(v >> (8 * i)));
}
inline std::uint32_t audio_read_u32(const std::string &s, std::size_t at) {
  if (at > s.size() || s.size() - at < 4) throw std::invalid_argument("incomplete audio header");
  std::uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) v |= std::uint32_t(static_cast<unsigned char>(s[at+i])) << (8*i);
  return v;
}
inline std::string audio_format(std::string value) {
  if (!value.empty() && value[0] == '.') value.erase(0, 1);
  if (value.empty() || value.size() > 16) throw std::invalid_argument("audio format must be a file extension");
  for (auto &c : value) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
      throw std::invalid_argument("audio format must be a file extension");
  }
  return value;
}
inline std::string audio_pcm(std::string bytes, const std::string &format,
    std::uint32_t rate, std::uint32_t channels, bool interleaved) {
  const unsigned width = format == "s16le" ? 2 : format == "f32le" ? 4 : 0;
  if (!width || rate < 8000 || rate > 192000 || channels < 1 || channels > 2)
    throw std::invalid_argument("PCM requires s16le/f32le, 8000..192000 Hz and mono/stereo");
  if (bytes.size() > kAudioMaxBytes || bytes.size() % (channels * width))
    throw std::invalid_argument("PCM must fit 16 MiB and contain whole audio frames");
  if (format == "f32le") for (std::size_t i = 0; i < bytes.size(); i += 4) {
    const auto bits = audio_read_u32(bytes, i); float sample;
    std::memcpy(&sample, &bits, 4);
    if (!std::isfinite(sample)) throw std::invalid_argument("PCM samples must be finite");
  }
  if (!interleaved && channels > 1) {
    std::string result(bytes.size(), '\0');
    const auto frames = bytes.size() / (channels * width);
    for (std::size_t f = 0; f < frames; ++f) for (unsigned c = 0; c < channels; ++c)
      std::memcpy(result.data() + (f * channels + c) * width,
                  bytes.data() + (c * frames + f) * width, width);
    return result;
  }
  return bytes;
}
inline std::string audio_wav(const std::string &pcm, const std::string &format,
    std::uint32_t rate, std::uint32_t channels) {
  (void)audio_pcm(pcm, format, rate, channels, true);
  if (pcm.size() > kAudioMaxBytes - 44) throw std::invalid_argument("WAV exceeds 16 MiB");
  const std::uint32_t width = format == "s16le" ? 2 : 4;
  auto u16 = [](std::string &s, std::uint16_t v) { s.push_back(char(v)); s.push_back(char(v >> 8)); };
  std::string s("RIFF"); audio_u32(s, 36 + pcm.size()); s += "WAVEfmt "; audio_u32(s, 16);
  u16(s, format == "s16le" ? 1 : 3); u16(s, channels); audio_u32(s, rate);
  audio_u32(s, rate * channels * width); u16(s, channels * width); u16(s, width * 8);
  s += "data"; audio_u32(s, pcm.size()); s += pcm; return s;
}

// A versioned inert binary display: fixed metadata, extension, then exact file
// bytes. No Unicode/JSON conversion and no VM or device handles in the bridge.
struct AudioDisplay {
  std::string format, bytes;
  double position = 0, volume = 1;
  bool waveform = true, loop = false, autoplay = false;
};

struct NotebookAudioInput { AudioDisplay audio; std::string name, revision; };
using NotebookAudioInputs = std::map<std::string, NotebookAudioInput>;
inline std::string audio_input_key(const std::string &channel) { return std::string(1, '\0') + "audio:" + channel; }
inline std::string audio_unbase64(const std::string &encoded) {
  if (encoded.size() > 24U * 1024U * 1024U || encoded.size() % 4) throw std::invalid_argument("invalid base64 audio size");
  auto digit = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+') return 62; if (c == '/') return 63; return -1;
  };
  std::string bytes;
  for (std::size_t i = 0; i < encoded.size(); i += 4) {
    const int a = digit(encoded[i]), b = digit(encoded[i+1]);
    const int c = encoded[i+2] == '=' ? 0 : digit(encoded[i+2]), d = encoded[i+3] == '=' ? 0 : digit(encoded[i+3]);
    const bool pad2 = encoded[i+2] == '=', pad1 = encoded[i+3] == '=';
    if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad1) || ((pad1 || pad2) && i + 4 != encoded.size()) ||
        (pad2 && (b & 15)) || (pad1 && !pad2 && (c & 3))) throw std::invalid_argument("invalid base64 audio data");
    bytes += static_cast<char>((a << 2) | (b >> 4));
    if (!pad2) bytes += static_cast<char>((b << 4) | (c >> 2));
    if (!pad1) bytes += static_cast<char>((c << 6) | d);
  }
  return bytes;
}

struct NotebookAudioComponent { std::string board, id, channel; std::uint64_t cell = 0; };
struct NotebookAudioControl {
  std::uint64_t cell = 0, playback_revision = 0, seek_revision = 0, volume_revision = 0;
  std::string channel;
  unsigned playback = 0; // unset, play, pause, stop
  double position = 0, volume = 1;
};
inline void audio_validate_control(const NotebookAudioControl &c) {
  if (!c.cell || c.channel.size() > 256 || c.channel.find('\0') != std::string::npos || c.playback > 3 ||
      !std::isfinite(c.position) || c.position < 0 || !std::isfinite(c.volume) || c.volume < 0 || c.volume > 1)
    throw std::invalid_argument("invalid audio player command");
}

// Ordered PCM transport. A replacement starts at frame zero; subsequent
// deltas start exactly at the receiver's retained frame count. Source tokens
// never repeat, including across run stores in the same worker process.
struct NotebookAudioEvent {
  std::uint64_t token = 0, revision = 0, offset = 0, frames = 0;
  std::uint32_t rate = 0, channels = 0;
  std::string sample_format, bytes;
  AudioDisplay player;
  bool replace = true, finished = false;
};
inline void audio_validate_event(const NotebookAudioEvent &e) {
  (void)audio_pcm(e.bytes, e.sample_format, e.rate, e.channels, true);
  const auto width = e.sample_format == "s16le" ? 2U : 4U;
  if (!e.token || !e.revision || e.offset > e.frames ||
      e.frames > (kAudioMaxBytes - 128) / (width * e.channels) ||
      e.bytes.size() / (width * e.channels) != e.frames - e.offset ||
      (e.replace && e.offset != 0) || !std::isfinite(e.player.position) || e.player.position < 0 ||
      !std::isfinite(e.player.volume) || e.player.volume < 0 || e.player.volume > 1)
    throw std::invalid_argument("invalid PCM stream event");
}
inline void audio_merge_event(NotebookAudioEvent &target, const NotebookAudioEvent &update) {
  audio_validate_event(update);
  if (update.replace || target.token != update.token) {
    if (!update.replace) throw std::invalid_argument("PCM stream requires a replacement before deltas");
    target = update; return;
  }
  if (update.revision <= target.revision) return; // replay after reconnect
  if (update.offset != target.frames || update.rate != target.rate ||
      update.channels != target.channels || update.sample_format != target.sample_format)
    throw std::invalid_argument("PCM stream gap or format change");
  const auto offset = target.offset; const auto replace = target.replace;
  auto bytes = std::move(target.bytes); bytes += update.bytes;
  target = update; target.bytes = std::move(bytes); target.offset = offset; target.replace = replace;
}
inline std::string audio_encode(const AudioDisplay &value) {
  const auto format = audio_format(value.format);
  if (value.bytes.empty() || value.bytes.size() > kAudioMaxBytes - 64 ||
      !std::isfinite(value.position) || value.position < 0 ||
      !std::isfinite(value.volume) || value.volume < 0 || value.volume > 1)
    throw std::invalid_argument("invalid audio payload or player position/volume");
  std::string out("SPKA1", 5);
  out.push_back(char((value.waveform ? 1 : 0) | (value.loop ? 2 : 0) | (value.autoplay ? 4 : 0)));
  audio_u32(out, format.size());
  auto real = [&](double v) { std::uint64_t bits; std::memcpy(&bits, &v, 8); for (unsigned i=0;i<8;++i) out.push_back(char(bits >> (8*i))); };
  real(value.position); real(value.volume); out += format; out += value.bytes; return out;
}
inline AudioDisplay audio_decode(const std::string &s) {
  if (s.size() < 28 || s.size() > kAudioMaxBytes || s.compare(0,5,"SPKA1"))
    throw std::invalid_argument("invalid audio display header");
  const auto size = audio_read_u32(s, 6);
  if (!size || size > 16 || s.size() <= 26 + size || (static_cast<unsigned char>(s[5]) & ~7U))
    throw std::invalid_argument("invalid audio display metadata");
  auto real = [&](unsigned at) { std::uint64_t bits=0; for(unsigned i=0;i<8;++i) bits |= std::uint64_t(static_cast<unsigned char>(s[at+i]))<<(8*i); double v; std::memcpy(&v,&bits,8); return v; };
  AudioDisplay out{audio_format(s.substr(26,size)),s.substr(26+size),real(10),real(18),bool(s[5]&1),bool(s[5]&2),bool(s[5]&4)};
  (void)audio_encode(out); return out;
}
}
