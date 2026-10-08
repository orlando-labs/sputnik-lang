#pragma once

#include "runtime/notebook_display.h"
#include <array>
#include <deque>
#include <iomanip>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>

namespace sputnik::runtime {

// The board stream is independent from plot.Figure rendering. Only inert
// numeric points and immutable style metadata cross to the render thread.
struct NotebookChartStyle {
  std::string id, caption, title, x_label, y_label;
  std::string stroke = "#d97706", background = "#ffffff";
  std::uint32_t width = 900, height = 420;
  double stroke_width = 2;
  std::uint64_t throttle_ms = 500;
  NotebookDisplayOrder order;
  std::optional<std::array<double, 4>> domain;
};
using NotebookChartPoint = std::array<double, 2>;
using NotebookChartChunk = std::vector<NotebookChartPoint>;
struct NotebookChartSnapshot {
  NotebookChartStyle style;
  std::uint64_t cell_id = 0, generation = 0, handle = 0, revision = 0;
  std::deque<std::shared_ptr<const NotebookChartChunk>> chunks;
  NotebookChartChunk tail;
  std::array<double, 4> bounds{};
  std::size_t count = 0;
  std::uint64_t total_points = 0;
};

inline std::string notebook_chart_quote(const std::string &text) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (unsigned char c : text) {
    if (c == '\"' || c == '\\') { result += '\\'; result += c; }
    else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
    else result += c;
  }
  return result + '"';
}

// Called by the background renderer, never by append_point. No Sputnik VM,
// heap, native extension, or device context participates in this work.
inline NotebookDisplay notebook_chart_render(const NotebookChartSnapshot &snapshot) {
  const auto &s = snapshot.style;
  auto b = snapshot.bounds;
  if (snapshot.count > 0) {
    bool first = true;
    auto include = [&](const NotebookChartPoint &p) {
      if (first) { b = {p[0], p[1], p[0], p[1]}; first = false; }
      else { b[0] = std::min(b[0], p[0]); b[1] = std::min(b[1], p[1]); b[2] = std::max(b[2], p[0]); b[3] = std::max(b[3], p[1]); }
    };
    for (const auto &chunk : snapshot.chunks) for (const auto &p : *chunk) include(p);
    for (const auto &p : snapshot.tail) include(p);
  }
  if (snapshot.count == 0) b = {0, 0, 1, 1};
  for (std::size_t axis = 0; axis < 2; ++axis) {
    const auto scale = std::max(std::abs(b[axis]), std::abs(b[axis + 2]));
    // The native viewer requires a numerically resolvable interval, not just
    // unequal endpoints. Expand near-constant and subnormal auto ranges too.
    if (b[axis + 2] - b[axis] <= std::max(1e-100, scale * 1e-13)) {
      const double center = (b[axis] + b[axis + 2]) / 2;
      const double d = std::max(0.5, scale * 1e-6);
      b[axis] = center - d; b[axis + 2] = center + d;
    }
  }
  if (s.domain) b = *s.domain;
  const double left = 56, top = 56 + (s.title.empty() ? 0 : 24);
  const double width = s.width - 112, height = s.height - top - 56;
  std::ostringstream out;
  out.imbue(std::locale::classic()); out << std::setprecision(16);
  out << "{\"version\":1,\"bounds\":[" << b[0] << ',' << b[1] << ',' << b[2] << ',' << b[3]
      << "],\"frame\":[" << left << ',' << top << ',' << width << ',' << height
      << "],\"padding\":56,\"color_scale\":false,\"aspect_equal\":false,\"y_down\":false,\"axes\":true,\"background\":"
      << notebook_chart_quote(s.background) << ",\"title\":" << notebook_chart_quote(s.title)
      << ",\"x_label\":" << notebook_chart_quote(s.x_label) << ",\"y_label\":" << notebook_chart_quote(s.y_label)
      << ",\"layers\":[{\"kind\":\"line\",\"commands\":[[2," << notebook_chart_quote(s.stroke)
      << ',' << s.stroke_width << ",1,[";
  bool first = true;
  auto point = [&](const NotebookChartPoint &p) {
    if (!first) out << ',';
    first = false;
    out << '[' << left + (p[0] - b[0]) / (b[2] - b[0]) * width << ','
        << top + (b[3] - p[1]) / (b[3] - b[1]) * height << ']';
  };
  for (const auto &chunk : snapshot.chunks) for (const auto &p : *chunk) point(p);
  for (const auto &p : snapshot.tail) point(p);
  out << "]]]}],\"decorations\":[]}";
  NotebookDisplay display;
  display.mime = "application/vnd.sputnik.plot+json";
  display.width = s.width; display.height = s.height;
  display.caption = s.caption; display.plot_scene = out.str();
  if (snapshot.total_points > snapshot.count)
    display.caption += " · latest " + std::to_string(snapshot.count) + " of " + std::to_string(snapshot.total_points) + " points";
  return display;
}
} // namespace sputnik::runtime
