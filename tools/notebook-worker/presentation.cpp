#include "tools/notebook-worker/presentation.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>

namespace amber::notebook::worker {
namespace {
constexpr const char *schema = "amber.notebook.presentation.v5";
constexpr std::size_t max_items = 65536;
constexpr std::size_t max_total_items = 262144;
struct Writer {
  std::string bytes;
  std::size_t total_items = 0;
  void space(std::size_t count) {
    if (count > kMaxPresentationBytes - bytes.size())
      throw ProtocolError("worker presentation exceeds 128 MiB limit");
  }
  template<class T> void number(T value) {
    if constexpr (std::numeric_limits<T>::is_signed)
      if (value < 0) throw ProtocolError("negative presentation integer");
    space(8);
    const auto n = static_cast<std::uint64_t>(value);
    for (unsigned i = 8; i > 0; --i) bytes.push_back(static_cast<char>(n >> ((i - 1) * 8)));
  }
  void str(const std::string &value) {
    number(value.size()); space(value.size()); bytes += value;
  }
  void real(double value) {
    if (!std::isfinite(value) || value < 0) throw ProtocolError("invalid progress number");
    std::uint64_t bits; std::memcpy(&bits, &value, sizeof(bits)); number(bits);
  }
  template<class T, class F> void items(const std::vector<T> &values, F visit) {
    if (values.size() > max_items || values.size() > max_total_items - total_items)
      throw ProtocolError("too many presentation items");
    total_items += values.size(); number(values.size());
    for (const auto &value : values) visit(*this, value);
  }
};
struct Reader {
  const std::string &bytes;
  std::size_t offset = 0, total_items = 0;
  template<class T> void number(T &value) {
    if (bytes.size() - offset < 8) throw ProtocolError("truncated presentation integer");
    std::uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i) n = (n << 8) | static_cast<unsigned char>(bytes[offset++]);
    if (n > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
      throw ProtocolError("invalid presentation integer");
    value = static_cast<T>(n);
  }
  void str(std::string &value) {
    std::uint64_t size; number(size);
    if (size > bytes.size() - offset) throw ProtocolError("truncated presentation string");
    value.assign(bytes, offset, static_cast<std::size_t>(size)); offset += size;
  }
  void real(double &value) {
    std::uint64_t bits; number(bits); std::memcpy(&value, &bits, sizeof(bits));
    if (!std::isfinite(value) || value < 0) throw ProtocolError("invalid progress number");
  }
  template<class T, class F> void items(std::vector<T> &values, F visit) {
    std::uint64_t count; number(count);
    if (count > max_items || count > max_total_items - total_items ||
        count > (bytes.size() - offset) / 8)
      throw ProtocolError("invalid presentation item count");
    total_items += count;
    // Decode each item before growing the result; untrusted lengths never
    // become an unchecked reserve/allocation.
    for (std::uint64_t i = 0; i < count; ++i) {
      T value; visit(*this, value); values.push_back(std::move(value));
    }
  }
};
template<class A, class T> void text_event(A &a, T &v) {
  a.str(v.stream); a.str(v.text); a.number(v.order);
  a.number(v.source.present); a.number(v.source.code_id); a.number(v.source.pc);
  a.str(v.source.file); a.number(v.source.line); a.number(v.source.column);
}
template<class A, class T> void local(A &a, T &v) {
  a.str(v.name); a.str(v.role); a.str(v.binding_kind); a.str(v.value); a.str(v.text_value);
  a.number(v.initialized); a.number(v.watched); a.number(v.watch_cell_id); a.number(v.watch_revision);
}
template<class A, class T> void range(A &a, T &v) {
  a.number(v.start_line); a.number(v.start_column); a.number(v.end_line);
  a.number(v.end_column); a.number(v.whole_line);
}
template<class A, class T> void display(A &a, T &v) {
  a.str(v.mime); a.str(v.caption); a.number(v.width); a.number(v.height); a.str(v.bytes);
  a.str(v.plot_scene);
  if (v.mime != "image/png" || !v.width || !v.height || v.width > 8192 || v.height > 8192 ||
      static_cast<std::uint64_t>(v.width) * v.height > 16000000 ||
      v.bytes.size() > 16U * 1024U * 1024U || v.plot_scene.size() > 4U * 1024U * 1024U)
    throw ProtocolError("invalid notebook display metadata");
  const auto u32 = [&](std::size_t offset) {
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n = (n << 8) | static_cast<unsigned char>(v.bytes[offset + i]);
    return n;
  };
  if (v.bytes.size() < 33 || v.bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0 ||
      v.bytes.compare(12, 4, "IHDR") != 0 || u32(16) != v.width || u32(20) != v.height)
    throw ProtocolError("invalid notebook PNG header");
}
template<class A, class T> void cell(A &a, T &v) {
  a.number(v.id); a.number(v.source_revision); a.number(v.dirty); a.number(v.ok);
  a.str(v.result); a.str(v.error); a.number(v.watch_epoch); a.number(v.watch_event_count);
  a.str(v.result_format.pretty); a.number(v.result_format.is_container);
  a.number(v.result_format.truncated); a.number(v.result_format.pretty_truncated);
  if (v.result.size() > 65536 || v.result_format.pretty.size() > 65536)
    throw ProtocolError("oversized notebook result preview");
  a.items(v.locals, [](auto &a, auto &v) { local(a, v); });
  a.items(v.output, [](auto &a, auto &v) { text_event(a, v); });
  a.items(v.displays, [](auto &a, auto &v) { display(a, v); });
  std::set<std::string> progress_ids;
  a.items(v.progress, [&](auto &a, auto &p) {
    a.str(p.id); a.real(p.current); a.real(p.total); a.str(p.description);
    a.number(p.done); a.number(p.elapsed_ms);
    if (p.id.empty() || p.id.size() > 4096 || p.description.size() > 65536 ||
        !progress_ids.insert(p.id).second) throw ProtocolError("invalid progress item");
  });
  a.items(v.error_ranges, [](auto &a, auto &v) { range(a, v); });
  a.items(v.errors, [](auto &a, auto &v) {
    a.str(v.message); a.number(v.has_range); range(a, v.range);
  });
  if (!v.id || v.displays.size() > 64 || v.progress.size() > 128) throw ProtocolError("invalid notebook cell presentation");
  std::size_t display_bytes = 0;
  for (const auto &display : v.displays) display_bytes += display.storage_bytes();
  if (display_bytes > 64U * 1024U * 1024U) throw ProtocolError("oversized notebook figure");
}
template<class A, class T> void presentation(A &a, T &v) {
  a.number(v.cancelled); a.str(v.status); a.str(v.environment_error); a.number(v.environment_stale);
  a.items(v.environment_output, [](auto &a, auto &v) { text_event(a, v); });
  std::set<std::uint64_t> failure_ids;
  a.items(v.unobserved_task_failures, [&](auto &a, auto &v) {
    a.number(v.task_id); a.str(v.error_name); a.str(v.message);
    auto &s = v.spawn_source;
    a.number(s.present); a.number(s.code_id); a.number(s.pc);
    a.str(s.file); a.number(s.line); a.number(s.column);
    if (!v.task_id || v.error_name.empty() || !failure_ids.insert(v.task_id).second)
      throw ProtocolError("invalid notebook task failure");
  });
  a.items(v.cells, [](auto &a, auto &v) { cell(a, v); });
  if (v.cells.size() > 4096) throw ProtocolError("too many notebook cells in presentation");
  std::set<CellId> ids;
  for (const auto &entry : v.cells)
    if (!ids.insert(entry.id).second) throw ProtocolError("duplicate presentation cell");
}
std::size_t size_field(const std::string &text) {
  std::size_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw ProtocolError("invalid presentation size/offset");
  return value;
}
}

Presentation presentation_from_session(const Session &session,
    const std::map<CellId, std::uint64_t> &source_revisions, bool cancelled) {
  Presentation result;
  result.cancelled = cancelled; result.status = session.status;
  result.environment_error = session.environment_error;
  result.environment_stale = session.environment_stale;
  result.environment_output = session.environment_output;
  for (const auto &cell : session.cells) {
    CellPresentation v;
    v.id = cell.id;
    const auto revision = source_revisions.find(cell.id);
    if (revision != source_revisions.end()) v.source_revision = revision->second;
    v.dirty = cell.dirty; v.ok = cell.ok; v.result = cell.result; v.error = cell.error;
    v.result_format = cell.result_format;
    v.locals = cell.locals; v.output = cell.output_events; v.displays = cell.displays;
    v.progress = cell.progress;
    v.watch_epoch = cell.watch_epoch; v.watch_event_count = cell.watch_event_count;
    v.error_ranges = cell.error_ranges; v.errors = cell.errors;
    result.cells.push_back(std::move(v));
  }
  return result;
}
std::string encode_presentation(const Presentation &value) {
  Writer writer;
  writer.str(schema); presentation(writer, value);
  return std::move(writer.bytes);
}
Presentation decode_presentation(const std::string &bytes) {
  if (bytes.size() > kMaxPresentationBytes) throw ProtocolError("oversized presentation");
  Reader reader{bytes};
  std::string version; reader.str(version);
  if (version != schema) throw ProtocolError("unsupported presentation schema");
  Presentation result; presentation(reader, result);
  if (reader.offset != bytes.size()) throw ProtocolError("trailing presentation bytes");
  return result;
}
void send_presentation(Channel &channel, std::uint64_t generation, std::uint64_t request,
                       const Presentation &value, std::chrono::milliseconds timeout, bool live) {
  const auto bytes = encode_presentation(value);
  channel.send({live ? Kind::LiveBegin : Kind::SnapshotBegin, generation, request, {schema, std::to_string(bytes.size())}}, timeout);
  for (std::size_t offset = 0; offset < bytes.size(); offset += kPresentationChunkBytes)
    channel.send({live ? Kind::LiveChunk : Kind::SnapshotChunk, generation, request,
                  {std::to_string(offset), bytes.substr(offset, kPresentationChunkBytes)}}, timeout);
  channel.send({live ? Kind::LiveEnd : Kind::Result, generation, request, {schema}}, timeout);
}
PresentationAssembler::PresentationAssembler(std::uint64_t generation, std::uint64_t request, bool live)
    : generation_(generation), request_(request), live_(live) {
  if (!generation || !request) throw ProtocolError("invalid presentation identity");
}
std::optional<Presentation> PresentationAssembler::accept(const Message &message) {
  try {
    if (state_ == State::Complete || state_ == State::Failed)
      throw ProtocolError("presentation assembler is closed");
    if (message.generation != generation_ || message.request != request_)
      throw ProtocolError("stale presentation identity");
    if (message.kind == (live_ ? Kind::LiveBegin : Kind::SnapshotBegin)) {
      if (state_ != State::Empty || message.fields.size() != 2 || message.fields[0] != schema)
        throw ProtocolError("invalid presentation begin");
      expected_ = size_field(message.fields[1]);
      if (!expected_ || expected_ > kMaxPresentationBytes) throw ProtocolError("invalid presentation size");
      state_ = State::Receiving;
    } else if (message.kind == (live_ ? Kind::LiveChunk : Kind::SnapshotChunk)) {
      if (state_ != State::Receiving || message.fields.size() != 2 ||
          size_field(message.fields[0]) != bytes_.size() || message.fields[1].empty() ||
          message.fields[1].size() > kPresentationChunkBytes ||
          message.fields[1].size() > expected_ - bytes_.size())
        throw ProtocolError("invalid presentation chunk");
      bytes_ += message.fields[1];
    } else if (message.kind == (live_ ? Kind::LiveEnd : Kind::Result)) {
      if (state_ != State::Receiving || message.fields != std::vector<std::string>{schema} || bytes_.size() != expected_)
        throw ProtocolError("incomplete presentation result");
      auto result = decode_presentation(bytes_);
      std::string{}.swap(bytes_); state_ = State::Complete;
      return result;
    } else throw ProtocolError("unexpected presentation message");
    return std::nullopt;
  } catch (...) {
    std::string{}.swap(bytes_); state_ = State::Failed;
    throw;
  }
}

RunOutputCapture::RunOutputCapture(Session &session)
    : session_(session), previous_(session.output_observer),
      sinks_(std::make_shared<std::vector<std::pair<CellId, Sinks>>>()) {
  session_.output_observer = [sinks = sinks_, previous = previous_](CellId id, auto out, auto err) {
    if (previous) previous(id, out, err);
    sinks->push_back({id, {std::move(out), std::move(err)}});
  };
}
RunOutputCapture::~RunOutputCapture() { session_.output_observer = std::move(previous_); }
void RunOutputCapture::finish() {
  // Keep every attempt until drain so even superseded writers are closed.
  // Iteration order makes the latest attempt's snapshot win for each cell.
  for (const auto &[id, sinks] : *sinks_) {
    std::vector<runtime::RuntimeTextOutputEvent> events;
    for (const auto &sink : {sinks.first, sinks.second}) {
      if (!sink) continue;
      sink->close();
      auto captured = sink->events();
      events.insert(events.end(), std::make_move_iterator(captured.begin()), std::make_move_iterator(captured.end()));
    }
    std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) { return a.order < b.order; });
    if (!id) session_.environment_output = std::move(events);
    else for (auto &cell : session_.cells) if (cell.id == id) { cell.output_events = std::move(events); break; }
  }
  sinks_->clear();
}
}
