class Reader:
  def read_bytes(path, limit: null): "user-defined reader"

def main():
  path = "tests/fixtures/native_fs_read_bytes/data.bin"
  bytes = fs.read_bytes(path, limit: 5)
  raise ValueError("binary file read") unless bytes.count == 5 and bytes[1] == 0
  raise ValueError("Path file read") unless fs.read_bytes(fs.Path(path)).hex == "6100620aff"
  raise ValueError("user method dispatch") unless Reader().read_bytes(path, limit: 5) == "user-defined reader"
  limited = false
  try:
    fs.read_bytes(path, limit: 4)
  rescue ArgumentError |error|:
    limited = true
  raise ValueError("limit must be enforced") unless limited
  missing = false
  try:
    fs.read_bytes("tests/fixtures/native_fs_read_bytes/absent.bin")
  rescue IOError |error|:
    missing = true
  raise ValueError("missing file error") unless missing
  invalid = false
  try:
    fs.read_bytes(path, limit: -1)
  rescue ArgumentError |error|:
    invalid = true
  raise ValueError("negative limit") unless invalid
  print "PASS native binary file read"
  0

main()
