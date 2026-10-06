package progressbar

export ProgressBar, ProgressEnumerable, with_progress

# A small, host-neutral progress bar. The runtime `notebook.progress` hook is
# deliberately optional: ordinary CLI programs get the stderr renderer while
# a notebook run receives the same absolute state through its live sink.
class ProgressBar:
  class_method def next_id():
    if @@next_id == null:
      @@next_id = 1
    else:
      @@next_id = @@next_id + 1
    "progress-" + @@next_id.to_str()

  def init(@total: null, @desc: "", @throttle: 0.2, @id: null):
    if @total != null and (not (Int === @total) or @total < 0):
      raise ArgumentError("progress total must be a nonnegative Int or null")

    if not (Int === @throttle or Float === @throttle):
      raise TypeError("progress throttle must be a nonnegative finite number")
    @throttle = @throttle.to_float()
    if @throttle != @throttle or @throttle < 0.0 or @throttle > 9e9:
      raise ArgumentError("progress throttle must be a nonnegative finite number")

    if @id == null:
      @id = ProgressBar.next_id
    elif not (Str === @id):
      raise TypeError("progress id must be a Str or null")

    @current = 0
    @ended = false
    @started_ns = Time.monotonic.total_nanoseconds
    @last_render_ns = @started_ns
    @throttle_ns = Math.floor(@throttle * 1000000000.0).to_int()
    render(false) unless publish(false)

  def current():
    @current

  def total():
    @total

  def desc():
    @desc

  def id():
    @id

  def ended?():
    @ended

  def inc!(amount = 1):
    if @ended:
      return @current
    if not Int === amount or amount < 0:
      raise ArgumentError("progress increment must be a nonnegative Int")
    if amount > 9223372036854775807 - @current:
      raise ArgumentError("progress counter overflow")

    @current = @current + amount
    now = Time.monotonic.total_nanoseconds
    if amount == 0 or @throttle_ns > 0 and now - @last_render_ns < @throttle_ns:
      return @current
    @last_render_ns = now
    render(false) unless publish(false)
    @current

  def end():
    if @ended:
      return @current
    @ended = true
    render(true) unless publish(true)
    @current

  # The hook returns false outside a notebook. Actual hook errors must not be
  # swallowed: silently losing the UI receiver makes failures invisible.
  def publish(done):
    total = @total
    total = 0 if total == null
    notebook.progress(@id, @current, total, @desc, done)

  # Render to the logical stderr stream. TTYs receive a single updating line;
  # redirected and notebook streams receive ordinary sparse lines, avoiding
  # terminal cursor escapes in captured output.
  def render(final):
    writer = io.stderr()
    line = text()
    if writer.xterm? and not final:
      writer.write_str("\r" + line)
      writer.flush()
    elif writer.xterm? and final:
      writer.write_str("\r" + line + "\n")
      writer.flush()
    else:
      writer.write_line(line)

  def text():
    prefix = ""
    if @desc != "":
      prefix = @desc + ": "
    if @total == null:
      prefix + @current.to_str()
    else:
      percent = 0
      if @total > 0:
        percent = Math.floor((@current.to_float() * 100.0) / @total.to_float()).to_int()
      prefix + @current.to_str() + "/" + @total.to_str() + " [" + percent.to_str() + "%]"

class ProgressEnumerable:
  def init(@source, @total: null, @desc: "", @throttle: 0.2, @id: null):
    null

  def each(&block):
    total = @total
    if total == null and (Array === @source or Tuple === @source or Range === @source or Set === @source):
      total = @source.count()

    bar = ProgressBar(total: total, desc: @desc,
                      throttle: @throttle, id: @id)
    try:
      @source.each |item|:
        value = block(item)
        bar.inc!()
        value
    ensure:
      bar.end

  def source():
    @source

  def total():
    @total

  def desc():
    @desc

def with_progress(source, total: null, desc: "", throttle: 0.2, id: null):
  ProgressEnumerable(source, total: total, desc: desc,
                     throttle: throttle, id: id)
