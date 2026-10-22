package notebook.ui

export publish_point, image, publish_image, publish_images, add_image, clear_images, read_image, read_images, DataTableView, AudioPlayer

class AudioPlayerHandle:
  def init(@scope, @cell, @channel): null
  def play(): notebook.audio_control(@scope, @cell, @channel, "play", null)
  def pause(): notebook.audio_control(@scope, @cell, @channel, "pause", null)
  def stop(): notebook.audio_control(@scope, @cell, @channel, "stop", null)
  def seek(seconds): notebook.audio_control(@scope, @cell, @channel, "seek", seconds)
  def set_volume(value): notebook.audio_control(@scope, @cell, @channel, "volume", value)
  def append(data): notebook.audio_control(@scope, @cell, @channel, "append", data)
  def finish(): notebook.audio_control(@scope, @cell, @channel, "finish", null)
  def reset(): notebook.audio_control(@scope, @cell, @channel, "reset", null)

class AudioPlayer:
  def init(@track, @position: 0.0, @volume: 1.0, @looped: false, @autoplay: false, @show_waveform: true):
    null

  class_method def for(track, position: 0.0, volume: 1.0, looped: false, autoplay: false, show_waveform: true):
    AudioPlayer(track, position: position, volume: volume, looped: looped,
      autoplay: autoplay, show_waveform: show_waveform)

  def notebook_audio():
    spec = @track.notebook_audio()
    spec.merge!({position: @position, volume: @volume, looped: @looped,
      autoplay: @autoplay, show_waveform: @show_waveform})
    spec

# Presentation owns no DB connection. The object can be exported outside a
# notebook; show captures its current data as an immutable, bounded snapshot.
class DataTableView:
  def init(@data, columns: null):
    raise ArgumentError("DataTableView data must be an Array or Tuple") unless Array === @data or Tuple === @data
    @selected_columns = columns

  def notebook_table():
    data = @data.to_array()
    records = data.count > 0 and Map === data[0]
    vectors = data.count > 0 and (Array === data[0] or Tuple === data[0])
    keys = []
    if @selected_columns != null:
      raise ArgumentError("columns must be an Array or Tuple") unless Array === @selected_columns or Tuple === @selected_columns
      keys = @selected_columns.to_array()
    else if records:
      data.each |row|:
        raise ArgumentError("DataTableView rows must have the same shape") unless Map === row
        row.keys.each |key|:
          keys.push!(key) unless keys.include?(key)
    else if vectors:
      width = 0
      data.each |row|:
        raise ArgumentError("DataTableView rows must have the same shape") unless Array === row or Tuple === row
        width = row.count if row.count > width
      i = 0
      while i < width:
        keys.push!("Column " + (i + 1).to_str())
        i = i + 1
    else:
      keys = ["Value"]
    raise ArgumentError("DataTableView requires at least one column") if keys.count == 0
    keys.each |key|:
      raise ArgumentError("column keys must be Str or Symbol") unless Str === key or Symbol === key
    names = keys.map: _1.to_str()
    raise ArgumentError("duplicate DataTableView columns") unless names.uniq().count == names.count
    rows = data.map |row|:
      if records:
        raise ArgumentError("DataTableView rows must have the same shape") unless Map === row
        keys.map: row[_1]
      else if vectors:
        raise ArgumentError("DataTableView rows must have the same shape") unless Array === row or Tuple === row
        raise ArgumentError("more row values than declared columns") if row.count > keys.count
        values = []
        i = 0
        while i < keys.count:
          values.push!(if i < row.count: row[i] else: null)
          i = i + 1
        values
      else:
        raise ArgumentError("scalar data requires one column") unless keys.count == 1
        raise ArgumentError("DataTableView rows must have the same shape") if Map === row or Array === row or Tuple === row
        [row]
    {columns: names, rows: rows}

  def to_csv():
    table = notebook_table()
    lines = [table[:columns].map: _csv_field(_1) .join(",")]
    table[:rows].each |row|:
      lines.push!(row.map: _csv_field(_1) .join(","))
    lines.join("\r\n") + "\r\n"

  def to_markdown():
    table = notebook_table()
    lines = [_markdown_row(table[:columns]), _markdown_row(table[:columns].map: "---")]
    table[:rows].each |row|:
      lines.push!(_markdown_row(row))
    lines.join("\n") + "\n"

  def _text(value):
    if value == null: "" else: value.to_str()

  def _csv_field(value):
    '"' + _text(value).replace('"', '""') + '"'

  def _markdown_row(row):
    cells = row.map |value|:
      _text(value).replace("\\", "\\\\").replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("|", "\\|").replace("\r\n", "\n").replace("\r", "\n").replace("\n", "<br>")
    "| " + cells.join(" | ") + " |"

# Publish numeric samples to a channel consumed by Live Chart components.
# The channel ID, layout and labels are declared in the board designer.
def publish_point(chart_id, x, y):
  notebook.chart_point(chart_id, x, y)

# PNG/JPEG Bytes infer dimensions; raw gray/rgb/rgba samples require them.
def image(data, width: null, height: null, format: :png, caption: ""):
  {data:, width:, height:, format:, caption:}

def publish_image(image_id, value, caption: ""):
  notebook.images(image_id, [value], caption:)

def publish_images(image_id, values, caption: ""):
  notebook.images(image_id, values, caption:)

# A null series caption preserves the current caption when appending.
def add_image(image_id, value, caption: null):
  notebook.images(image_id, [value], caption:, append: true)

def clear_images(image_id):
  notebook.images(image_id, [])

# Reading must be enabled in the board component's settings.
# Descriptors contain encoded Bytes, dimensions, format and frame caption.
def read_image(image_id, index: null):
  notebook.read_image(image_id, index:)

def read_images(image_id):
  notebook.read_images(image_id)
