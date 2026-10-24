import io
from io import ByteBuffer
import task

export main

def check(condition, message):
  raise ValueError(message) unless condition

def bytes_are(value, text): Hex.encode(value) == Hex.encode(Bytes.new(text))

def main():
  buffer = io.ByteBuffer(4)
  check(buffer.capacity == 4 and buffer.position == 0 and buffer.remaining == 4, "short constructor")
  buffer.put!(65).put_all!(Bytes.new("BC"))
  check(buffer.count == 3 and bytes_are(buffer.bytes, "ABC"), "byte writes")
  buffer.put!(68)
  check(buffer.full? and bytes_are(buffer.copy_bytes, "ABCD"), "full buffer")
  buffer.flip!
  check(buffer.get! == 65 and buffer.get_at(1) == 67, "indexed and sequential read")
  slice = buffer.read_slice(2)
  check(slice.count == 2 and bytes_are(slice.bytes, "BC"), "read slice")
  check(not slice.shareable? and slice.owner.capacity == 4, "slice retains buffer owner")
  buffer.rewind!
  check(buffer.get! == 65, "rewind")
  check(bytes_are(buffer.slice(-2).copy_bytes, "CD"), "negative slice")
  buffer.compact!
  check(bytes_are(buffer.bytes, "BCD"), "compact")
  check(buffer.clear! == null and buffer.empty?, "clear returns Null")
  check(buffer.write_slice(2).count == 2, "write slice")
  buffer.put_all!("xy")
  check(bytes_are(buffer.freeze_bytes!, "xy"), "freeze bytes")

  explicit = io.ByteBuffer.new(2)
  explicit.put_all!("ok")
  check(bytes_are(explicit.bytes, "ok"), "explicit constructor")
  imported = ByteBuffer.new(0)
  check(imported.capacity == 0, "imported constructor")
  wrapped = io.ByteBuffer.wrap(Bytes.new("wrapped"))
  check(wrapped.get! == 119, "wrapped buffer starts in read mode")
  copied = io.ByteBuffer(7)
  copied.put_all!(wrapped)
  check(bytes_are(copied.bytes, "rapped"), "buffer input copies active bytes")
  copied.clear!
  copied.put_all!(wrapped.read_slice(2))
  check(bytes_are(copied.bytes, "ra"), "slice input")

  rejected = false
  try:
    io.ByteBuffer(-1)
  rescue ArgumentError:
    rejected = true
  check(rejected, "negative capacity is rescuable")
  rejected = false
  try:
    io.ByteBuffer("4")
  rescue TypeError:
    rejected = true
  check(rejected, "noninteger capacity is rescuable")
  rejected = false
  try:
    io.ByteBuffer(0).put!(1)
  rescue ArgumentError:
    rejected = true
  check(rejected, "overflow is rescuable")
  child = task.spawn:
    try:
      explicit.put!(1)
      false
    rescue IsolationError:
      true
  check(child.wait(), "buffer remains confined to its task")
  print "PASS native byte buffer"
  0

main()
