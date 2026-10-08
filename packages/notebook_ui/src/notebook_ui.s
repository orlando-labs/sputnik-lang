package notebook_ui

export LiveChart

# A notebook/board component stream, separate from synchronous plot.Figure
# export. append! publishes one numeric pair; the runtime renders snapshots
# asynchronously and the native viewer draws the resulting scene.
class LiveChart:
  def init(id:, title: "", x_label: "", y_label: "", caption: "",
      width: 900, height: 420, stroke: "#d97706", stroke_width: 2,
      throttle_ms: 500, order: 0, domain: null):
    @handle = notebook.chart_begin(id, title:, x_label:, y_label:, caption:,
      width:, height:, stroke:, stroke_width:, throttle_ms:, order:, domain:)
    @ended = false

  def append!(x, y):
    raise ValueError("chart has ended") if @ended
    notebook.chart_point(@handle, x, y)
    self

  def end():
    unless @ended:
      notebook.chart_end(@handle)
      @ended = true
    self
