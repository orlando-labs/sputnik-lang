package audio
from notebook.ui import AudioCodec

export AudioTrack

# Audio data can be constructed/exported without a notebook or an audio device.
class AudioTrack:
  def init(@data, @format, @sample_format: null, @sample_rate: null, @channels: null, @stream_token: null):
    null

  class_method def from_input(channel):
    spec = notebook.read_audio(channel)
    AudioTrack.from_data(spec[:data], format: spec[:format])

  class_method def from_file(path, format: null):
    if format == null:
      parts = path.to_str().split(".")
      raise ArgumentError("audio file without extension requires format") if parts.count < 2
      format = parts[-1]
    AudioTrack.from_data(fs.read_bytes(path, limit: 16777153), format: format)

  class_method def from_data(data, format:):
    AudioTrack(AudioCodec.copy(data), AudioCodec.extension(format))

  class_method def from_pcm(data, sample_format:, sample_rate:, channels: null, layout: null, interleaved: true):
    if layout != null:
      count = if layout == :mono: 1 else: if layout == :stereo: 2 else: 0
      raise ArgumentError("layout must be mono or stereo") if count == 0
      raise ArgumentError("channels and layout disagree") if channels != null and channels != count
      channels = count
    raise ArgumentError("PCM requires channels or layout") if channels == null
    pcm = AudioCodec.pcm(data, sample_format, sample_rate, channels, interleaved)
    AudioTrack(pcm, "pcm", sample_format: sample_format, sample_rate: sample_rate, channels: channels)

  class_method def stream(sample_format:, sample_rate:, channels: null, layout: null):
    if layout != null:
      count = if layout == :mono: 1 else: if layout == :stereo: 2 else: 0
      raise ArgumentError("layout must be mono or stereo") if count == 0
      raise ArgumentError("channels and layout disagree") if channels != null and channels != count
      channels = count
    raise ArgumentError("PCM requires channels or layout") if channels == null
    token = AudioCodec.stream(sample_format, sample_rate, channels)
    AudioTrack(null, "pcm", sample_format: sample_format, sample_rate: sample_rate, channels: channels, stream_token: token)

  def append(data):
    raise ArgumentError("append requires AudioTrack.stream") if @stream_token == null
    AudioCodec.append(@stream_token, data)
    self

  def finish():
    raise ArgumentError("finish requires AudioTrack.stream") if @stream_token == null
    AudioCodec.finish(@stream_token)
    self

  def reset():
    raise ArgumentError("reset requires AudioTrack.stream") if @stream_token == null
    AudioCodec.reset(@stream_token)
    self

  def data(): if @stream_token == null: @data else: AudioCodec.stream_data(@stream_token)
  def format(): @format
  def asset_id(): AudioCodec.asset_id(if @sample_format == null: data() else: to_wav())

  def to_pcm(sample_format: :f32le):
    if @sample_format != null:
      data = AudioCodec.convert(data(), @sample_format, sample_format, @sample_rate, @channels)
      AudioTrack.from_pcm(data, sample_format: sample_format, sample_rate: @sample_rate, channels: @channels)
    else:
      decoded = AudioCodec.decode(@data, @format)
      data = AudioCodec.convert(decoded["data"], :f32le, sample_format, decoded["sample_rate"], decoded["channels"])
      AudioTrack.from_pcm(data, sample_format: sample_format, sample_rate: decoded["sample_rate"], channels: decoded["channels"])

  def to_wav(sample_format: null):
    if @format == "wav" and @sample_format == null and sample_format == null: return @data
    target = if sample_format == null: if @sample_format == null: :f32le else: @sample_format else: sample_format
    if @sample_format != null:
      data = AudioCodec.convert(data(), @sample_format, target, @sample_rate, @channels)
      AudioCodec.wav(data, target, @sample_rate, @channels)
    else:
      to_pcm(sample_format: target).to_wav()

  def notebook_audio():
    if @stream_token != null: return {stream: @stream_token}
    if @sample_format == null:
      {data: @data, format: @format}
    else:
      {data: to_wav(), format: "wav"}
