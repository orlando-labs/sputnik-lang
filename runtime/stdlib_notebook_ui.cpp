#include "runtime/stdlib_registry.h"
#include "runtime/notebook_live.h"
#include "runtime/audio.h"
#include "runtime/audio_system.h"

namespace sputnik::runtime {
namespace {
SendStatus dispatch_AudioCodec(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.reject_unknown_keywords({})) return SendStatus::Faulted;
  try {
    if (call.selector == "asset_id" && call.args.size() == 1) {
      const auto bytes = call.args[0].is_string() ? call.text_of(call.args[0]) : call.bytes_of(call.args[0]);
      if (!bytes) return SendStatus::Faulted;
      *call.out = call.string_value(notebook_asset_id(*bytes));
    } else if (call.selector == "stream" || call.selector == "append" || call.selector == "finish" || call.selector == "reset" || call.selector == "stream_data") {
      const auto store = NotebookLiveScope::current();
      if (!store) return call.fault("NotebookCellError", "audio stream requires an active notebook run");
      if (call.selector == "stream") {
        if (call.args.size() != 3 || !call.args[1].is_integer() || !call.args[2].is_integer())
          return call.fault("TypeError", "stream requires sample format, integer rate and channels");
        const auto format = call.text_of(call.args[0]);
        const auto rate = call.args[1].as_integer(), channels = call.args[2].as_integer();
        if (!format || rate < 8000 || rate > 192000 || channels < 1 || channels > 2)
          throw std::invalid_argument("invalid PCM stream format/rate/channels");
        *call.out = Value::integer(store->begin_audio(NotebookLiveScope::cell_id(), NotebookLiveScope::cell_generation(), *format, rate, channels));
      } else {
        const auto expected = call.selector == "append" ? 2U : 1U;
        if (call.args.size() != expected || !call.args[0].is_integer() || call.args[0].as_integer() <= 0)
          return call.fault("TypeError", "invalid audio stream handle/arguments");
        const auto token = call.args[0].as_integer();
        if (call.selector == "append") {
          const auto bytes = call.args[1].is_string() ? call.text_of(call.args[1]) : call.bytes_of(call.args[1]);
          if (!bytes) return SendStatus::Faulted;
          store->append_audio(token, *bytes);
        } else if (call.selector == "finish") store->finish_audio(token);
        else if (call.selector == "reset") store->reset_audio(token);
        else { *call.out = call.bytes_value(store->audio_data(token)); return SendStatus::Matched; }
        *call.out = Value::null();
      }
    } else if (call.selector == "decode" && call.args.size() == 2) {
      const auto bytes = call.args[0].is_string() ? call.text_of(call.args[0]) : call.bytes_of(call.args[0]);
      const auto format = call.text_of(call.args[1]);
      if (!bytes || !format) return call.fault("TypeError", "decode requires audio data and a file extension");
      const auto decoded = audio_system_decode(*bytes,*format);
      *call.out = call.make_object({{"data",call.bytes_value(decoded.bytes)}, {"sample_rate",Value::integer(decoded.rate)}, {"channels",Value::integer(decoded.channels)}});
    } else if (call.selector == "convert" && call.args.size() == 5) {
      const auto bytes = call.bytes_of(call.args[0]);
      const auto from = call.text_of(call.args[1]), to = call.text_of(call.args[2]);
      if (!bytes || !from || !to || !call.args[3].is_integer() || !call.args[4].is_integer())
        return call.fault("TypeError", "convert requires PCM bytes, sample formats, rate and channels");
      const auto rate = call.args[3].as_integer(), channels = call.args[4].as_integer();
      if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2) throw std::invalid_argument("invalid PCM rate/channels");
      *call.out = call.bytes_value(audio_convert_pcm(*bytes,*from,*to,rate,channels));
    } else if (call.selector == "extension" && call.args.size() == 1) {
      const auto value = call.text_of(call.args[0]);
      if (!value) return call.fault("TypeError", "audio format must be text");
      *call.out = call.string_value(audio_format(*value));
    } else if (call.selector == "copy" && call.args.size() == 1) {
      const auto value = call.args[0].is_string() ? call.text_of(call.args[0]) : call.bytes_of(call.args[0]);
      if (!value) return SendStatus::Faulted;
      if (value->empty() || value->size() > kAudioMaxBytes - 64) throw std::invalid_argument("audio file must contain 1..16777152 bytes");
      *call.out = call.bytes_value(*value);
    } else if ((call.selector == "pcm" && call.args.size() == 5) || (call.selector == "wav" && call.args.size() == 4)) {
      const auto bytes = call.args[0].is_string() ? call.text_of(call.args[0]) : call.bytes_of(call.args[0]);
      const auto format = call.text_of(call.args[1]);
      if (!bytes || !format || !call.args[2].is_integer() || !call.args[3].is_integer())
        return call.fault("TypeError", "PCM requires binary data, sample format and integer rate/channels");
      const auto rate = call.args[2].as_integer(), channels = call.args[3].as_integer();
      if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2) throw std::invalid_argument("invalid PCM rate/channels");
      if (call.selector == "pcm") {
        if (!call.args[4].is_bool()) return call.fault("TypeError", "interleaved must be Bool");
        *call.out = call.bytes_value(audio_pcm(*bytes, *format, rate, channels, call.args[4].as_bool()));
      } else *call.out = call.bytes_value(audio_wav(*bytes, *format, rate, channels));
    } else return SendStatus::NotHandled;
    return SendStatus::Matched;
  } catch (const std::exception &e) { return call.fault("ArgumentError", e.what()); }
}
SendStatus dispatch_stopwatch(NativeStdlibCall &call) {
  if (call.selector != "measure") return SendStatus::NotHandled;
  if (!call.reject_unknown_keywords({})) return SendStatus::Faulted;
  if (call.block.is_null() || call.args.size() > 1)
    return call.fault("TypeError", "Stopwatch.measure accepts an optional channel ID and a required block");
  const auto id = call.args.empty() ? std::optional<std::string>("default") : call.text_of(call.args[0]);
  if (!id) return call.fault("TypeError", "Stopwatch ID must be Str or Symbol");
  const auto store = NotebookLiveScope::current();
  if (!store) return call.fault("NotebookCellError", "Stopwatch.measure requires an active notebook run");
  std::uint64_t token;
  try { token = store->begin_stopwatch(NotebookLiveScope::cell_id(), NotebookLiveScope::cell_generation(), *id); }
  catch (const std::exception &error) { return call.fault("ArgumentError", error.what()); }
  struct Finish {
    std::shared_ptr<NotebookLiveStore> store; std::uint64_t token;
    ~Finish() { store->end_stopwatch(token); }
  } finish{store, token};
  const auto result = call.call_block(call.block, {});
  if (result.status == StdlibBlockStatus::Returned) { *call.out = result.value; return SendStatus::Matched; }
  if (result.status == StdlibBlockStatus::Raised) return call.raise(result.exception);
  return SendStatus::Faulted;
}
RuntimeNativeModuleDescriptor descriptor() {
  return {{{"notebook.ui.Stopwatch", RuntimeNativeTypeKind::NotebookStopwatch}, {"notebook.ui.AudioCodec", RuntimeNativeTypeKind::AudioCodec}},
          {{RuntimeNativeTypeKind::NotebookStopwatch, &dispatch_stopwatch}, {RuntimeNativeTypeKind::AudioCodec, &dispatch_AudioCodec}}, {}, {}, {}};
}
}
void register_notebook_ui(NativeRegistry &registry) { register_native_module_descriptor(registry, descriptor()); }
void register_notebook_ui_runtime_module(RuntimeModuleRegistry &modules, RuntimeDispatchRegistry &dispatch, RuntimeTypeRegistry &types) {
  const auto value = descriptor();
  register_runtime_module_descriptor(modules, value);
  register_runtime_dispatch_descriptor(dispatch, value);
  register_runtime_type_descriptor(types, value);
}
}
