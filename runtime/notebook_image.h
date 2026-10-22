#pragma once
#include "runtime/notebook_display.h"
#include "runtime/digest.h"
#include <optional>
#include <map>
#include <utility>

namespace sputnik::runtime {
inline std::string notebook_asset_id(const std::string &bytes) {
  const auto hash = digest_sha256(bytes); static const char *hex = "0123456789abcdef";
  std::string out; out.reserve(64);
  for (const unsigned char byte : hash) { out += hex[byte >> 4]; out += hex[byte & 15]; }
  return out;
}

// Immutable board inputs captured by a cell, independent of any VM heap.
struct NotebookImageInput {
  std::vector<NotebookDisplay> images;
  std::string caption;
  std::string revision;
  std::size_t selected = 0;
  bool readable = false;
};
using NotebookImageInputs = std::map<std::string, NotebookImageInput>;

// Inert image metadata shared by the VM and worker decoder.
inline std::pair<std::uint32_t, std::uint32_t> notebook_image_dimensions(
    const std::string &bytes, const std::string &mime) {
  auto u32 = [&](std::size_t i) {
    std::uint32_t n = 0;
    for (unsigned j = 0; j < 4; ++j) n = (n << 8) | static_cast<unsigned char>(bytes.at(i + j));
    return n;
  };
  if (mime == "image/png" && bytes.size() >= 33 &&
      bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0 &&
      u32(8) == 13 && bytes.compare(12, 4, "IHDR") == 0)
    return {u32(16), u32(20)};
  if (mime == "image/jpeg" && bytes.size() >= 4 &&
      static_cast<unsigned char>(bytes[0]) == 255 && static_cast<unsigned char>(bytes[1]) == 216) {
    std::size_t i = 2;
    while (i + 3 < bytes.size()) {
      if (static_cast<unsigned char>(bytes[i++]) != 255) break;
      while (i < bytes.size() && static_cast<unsigned char>(bytes[i]) == 255) ++i;
      if (i >= bytes.size()) break;
      const auto marker = static_cast<unsigned char>(bytes[i++]);
      if (marker == 217 || marker == 218) break;
      if (marker == 1 || (marker >= 208 && marker <= 215)) continue;
      if (i + 2 > bytes.size()) break;
      const auto length = (static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1]);
      if (length < 2 || std::size_t(length) > bytes.size() - i) break;
      if (marker >= 192 && marker <= 207 && marker != 196 && marker != 200 && marker != 204 && length >= 8) {
        const auto h = (static_cast<unsigned char>(bytes[i + 3]) << 8) | static_cast<unsigned char>(bytes[i + 4]);
        const auto w = (static_cast<unsigned char>(bytes[i + 5]) << 8) | static_cast<unsigned char>(bytes[i + 6]);
        return {w, h};
      }
      i += length;
    }
  }
  throw std::invalid_argument("invalid notebook PNG/JPEG header");
}

inline void notebook_validate_image(const NotebookDisplay &image) {
  if (!image.width || !image.height || image.width > 8192 || image.height > 8192 ||
      std::uint64_t(image.width) * image.height > 16000000 ||
      image.bytes.size() > 16U * 1024U * 1024U || image.caption.size() > 65536 ||
      notebook_image_dimensions(image.bytes, image.mime) != std::make_pair(image.width, image.height))
    throw std::invalid_argument("invalid notebook image dimensions or payload");
}

// Raw pixels become portable PNGs at the runtime boundary. Stored DEFLATE
// blocks avoid an image-library dependency in the language runtime.
inline std::string notebook_pixels_png(const std::string &pixels,
    std::uint32_t width, std::uint32_t height, unsigned channels) {
  if (!width || !height || width > 8192 || height > 8192 ||
      std::uint64_t(width) * height > 16000000 ||
      (channels != 1 && channels != 3 && channels != 4) ||
      pixels.size() != std::uint64_t(width) * height * channels || pixels.size() > 16U * 1024U * 1024U)
    throw std::invalid_argument("raw image requires width × height × channels byte samples");
  auto put32 = [](std::string &out, std::uint32_t n) {
    for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>(n >> shift);
  };
  auto chunk = [&](std::string &out, const char *kind, const std::string &data) {
    put32(out, data.size());
    const auto begin = out.size(); out.append(kind, 4); out += data;
    std::uint32_t crc = 0xffffffff;
    for (std::size_t i = begin; i < out.size(); ++i) {
      crc ^= static_cast<unsigned char>(out[i]);
      for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    put32(out, ~crc);
  };
  std::string scanlines;
  for (std::uint32_t row = 0; row < height; ++row) {
    scanlines += '\0'; scanlines.append(pixels, std::size_t(row) * width * channels, std::size_t(width) * channels);
  }
  std::string compressed("\x78\x01", 2);
  for (std::size_t offset = 0; offset < scanlines.size();) {
    const auto count = std::min<std::size_t>(65535, scanlines.size() - offset);
    compressed += static_cast<char>(offset + count == scanlines.size());
    compressed += static_cast<char>(count); compressed += static_cast<char>(count >> 8);
    compressed += static_cast<char>(~count); compressed += static_cast<char>((~count) >> 8);
    compressed.append(scanlines, offset, count); offset += count;
  }
  std::uint32_t a = 1, b = 0;
  for (unsigned char c : scanlines) { a = (a + c) % 65521; b = (b + a) % 65521; }
  put32(compressed, (b << 16) | a);
  std::string result("\x89PNG\r\n\x1a\n", 8), header;
  put32(header, width); put32(header, height);
  header += '\x08'; header += static_cast<char>(channels == 1 ? 0 : channels == 3 ? 2 : 6);
  header.append(3, '\0');
  chunk(result, "IHDR", header); chunk(result, "IDAT", compressed); chunk(result, "IEND", "");
  return result;
}
} // namespace sputnik::runtime
