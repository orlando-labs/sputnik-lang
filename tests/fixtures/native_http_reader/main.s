package native.http_reader

from net.http import Client, Server, ServerResponse, RequestBody

export main

def check(value, message):
  raise ValueError(message) unless value

def main():
  path = "/tmp/sputnik-reader-" + SecureRandom.hex(8)
  fs.File.open(path, :write, create: true, exclusive: true, permissions: 384) |file|:
    16384.times:
      file.write_all!("part\n".bytes)
    file.sync!()
  fs.File.open(path, :read) |file|:
    total = 0
    chunks = 0
    result = file.each_chunk(size: 1024, timeout: 5.0) |chunk|:
      check(Bytes === chunk and chunk.count > 0 and chunk.count <= 1024, "file chunks are bounded Bytes")
      total += chunk.count
      chunks += 1
    check(result == null and total == 81920 and chunks > 1, "file each_chunk reads through EOF")
  opened = null
  interrupted = false
  try:
    fs.File.open(path, :read) |file|:
      opened = file
      file.each_chunk(size: 1024) |chunk|: raise ValueError("interrupted reader")
  rescue ValueError:
    interrupted = true
  check(interrupted and opened.closed?, "file each_chunk propagates callback errors through scoped cleanup")
  server = Server(host: "127.0.0.1", port: 0, workers: 1)
  runner = task.spawn:
    server.serve(max_requests: 3) |request|:
      total = 0
      while chunk = request.body_stream.read_chunk(max_bytes: 65536):
        total += chunk.bytes.count
      ServerResponse.text(total.to_str)
  client = Client(timeout: 5.0)
  url = "http://127.0.0.1:" + server.port.to_str + "/upload"
  try:
    fs.File.open(path, :read) |file|:
      response = client.put(url, headers: {connection: "close"}, body: RequestBody.from_reader(file, length: 81920))
      check(response.body_text == "81920", "streamed fixed body must include all file chunks")
    fs.File.open(path, :read) |file|:
      response = client.put(url, headers: {connection: "close"}, body: RequestBody.from_reader(file, length: null))
      check(response.body_text == "81920", "streamed chunked body must finish framing")
    empty = path + "-empty"
    fs.File.open(empty, :write, create: true, exclusive: true, permissions: 384) |file|:
      file.write_all!("".bytes)
    fs.File.open(empty, :read) |file|:
      count = 0
      file.each_chunk |chunk|:
        count += 1
      check(count == 0, "empty file each_chunk does not invoke the callback")
      response = client.put(url, headers: {connection: "close"}, body: RequestBody.from_reader(file, length: 0))
      check(response.body_text == "0", "empty reader must send length zero")
    fs.remove(empty)
    runner.wait()
  ensure:
    client.close!()
    server.close!()
    fs.remove(path)
  true
