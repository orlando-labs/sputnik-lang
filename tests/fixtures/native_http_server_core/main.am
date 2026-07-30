package native.http_server_core

from net.http import Server, ServerRequest, ServerResponse

export main

def inspect_request(request as ServerRequest):
  chunk = request.body_stream().read_chunk(max_bytes: 8)
  if chunk:
    chunk.extensions().count + request.headers().to_pairs().count
  else:
    0

def streaming_response():
  ServerResponse.stream(
    headers: {"content-type": "text/plain"},
    trailers: ["x-native-done"]) |writer|:
      writer.write("ok")
      writer.close()
      writer.trailer("x-native-done", "yes")
      writer.finish()

def main():
  count = [Server, ServerRequest, ServerResponse].count
  count += if ServerRequest === {} then 100 else 0
  response = ServerResponse(
    status: 201,
    headers: {"x-native": "yes"},
    body: "ok")
  response_valid = response.status() == 201
    and response.body_text() == "ok"
    and response.headers().first("x-native") == "yes"
  if response_valid:
    count += 4
  else:
    count += 100
  count
