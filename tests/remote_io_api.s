package test.remote_io_api


export main

def check(condition, message): raise ValueError(message) unless condition

def main():
  root = "/tmp/sp-remote-io-" + SecureRandom.hex(8)
  fs.mkdir(root)
  try:
    path = root + "/snapshot"
    fs.atomic_write_text(path, "first")
    fs.atomic_write_bytes(path, "second".bytes)
    check(fs.read_text(path) == "second", "atomic replacement")
    fs.atomic_write_text(path, "third", durable: false)
    check(fs.read_text(path) == "third", "non-durable replacement")
    check(fs.sync(path) == null and fs.sync(fs.Path(root)) == null, "sync file and directory")
    caught = false
    try: fs.sync(root + "/missing")
    rescue IOError: caught = true
    check(caught and not fs.exists?(root + "/missing"), "sync never creates its target")
    caught = false
    try: fs.sync(42)
    rescue TypeError: caught = true
    check(caught, "sync path type")
    caught = false
    try: fs.sync("")
    rescue ArgumentError: caught = true
    check(caught, "sync empty path")
    caught = false
    try: fs.sync(root + "\u{0}ignored")
    rescue ArgumentError: caught = true
    check(caught, "sync rejects embedded NUL")
    caught = false
    try: fs.sync(root, durable: true)
    rescue TypeError: caught = true
    check(caught, "sync rejects keywords")
    caught = false
    try: fs.sync(root): raise ValueError("sync block must not execute")
    rescue TypeError: caught = true
    check(caught, "sync rejects block")
    caught = false
    try: fs.sync()
    rescue TypeError: caught = true
    check(caught, "sync requires a path")
    caught = false
    try: fs.sync(root, root)
    rescue TypeError: caught = true
    check(caught, "sync accepts one path")
    fifo = root + "/fifo"
    system.command("/usr/bin/mkfifo", fifo).run()
    caught = false
    try: fs.sync(fifo)
    rescue ArgumentError: caught = true
    check(caught, "sync rejects FIFO without waiting for a writer")
    fs.remove(fifo)
    if fs.exists?("/proc/self/status"):
      caught = false
      try: fs.sync("/proc/self/status")
      rescue IOError: caught = true
      check(caught, "sync propagates filesystem fsync failure")
    fs.lock(root + "/lock") |lock|:
      caught = false
      try: fs.lock(root + "/lock")
      rescue IOError: caught = true
      check(caught, "exclusive lock contention")
    fs.lock(root + "/lock") |lock|: check(not lock.closed?, "released scoped lock")
    first = fs.lock(root + "/lock", exclusive: false)
    second = fs.lock(root + "/lock", exclusive: false)
    first.close!()
    second.close!()
    symlink = root + "/alias"
    system.command("/bin/ln", "-s", path, symlink).run()
    entries = fs.entries(root)
    check(entries.count == 3 and fs.metadata(symlink).symlink?, "entries preserve symlinks")
    caught = false
    try: fs.sync(symlink)
    rescue IOError: caught = true
    check(caught, "sync rejects final symlink")
    fs.atomic_write_text(symlink, "replacement")
    check(fs.read_text(path) == "third" and fs.read_text(symlink) == "replacement", "atomic write replaces symlink")
    caught = false
    try: Json.parse("{\"nested\":{\"x\":1,\"\\u0078\":2}}", duplicate_keys: :reject)
    rescue JsonParseError: caught = true
    check(caught, "nested duplicate escaped JSON key")
    check(Json.parse("{\"x\":1,\"x\":2}", duplicate_keys: :last)[:x] == 2, "compatible duplicate default")
    check(Json.parse("{\"x\":1}", duplicate_keys: :reject, map: StrictMap)["x"] == 1, "strict map parsing")
    check(not (StrictMap === Json.parse("{}", map: Map, strict: true, duplicate_keys: :reject)), "map option precedence")
    check(Float === Json.parse("9223372036854775808", duplicate_keys: :reject), "JSON integer overflow matches VM")
    caught = false
    try: Json.parse("\u{000b}1", duplicate_keys: :reject)
    rescue JsonParseError: caught = true
    check(caught, "JSON permits only its four whitespace characters")
    socket_path = root + "/agent.sock"
    listener = net.unix.listen(socket_path, isolation: :unchecked)
    served = task.async:
      peer = listener.accept!(timeout: 5.0)
      check(peer.local_path == socket_path, "accepted Unix endpoint")
      check(peer.read_exact!(4, timeout: 5.0).to_str == "ping", "Unix read")
      first_line = peer.read_line!(timeout: 5.0)
      check(Str === first_line and first_line == "one", "buffered first line is Str")
      check(peer.read_line!(timeout: 5.0).to_str == "two", "buffered second line")
      check(peer.read_line!(timeout: 5.0) == null, "Unix write shutdown")
      peer.write_all!("pong".bytes, timeout: 5.0)
      peer.flush!()
      peer.close!()
    net.unix.connect(socket_path, timeout: 5.0) |socket|:
      check(socket.remote_path == socket_path, "Unix peer endpoint")
      socket.write_all!("pingone\ntwo\n".bytes, timeout: 5.0)
      socket.shutdown!(:write)
      check(socket.read_exact!(4, timeout: 5.0).to_str == "pong", "Unix roundtrip")
    served.wait()
    # Closing must not unlink a file that replaced the original socket inode.
    fs.remove(socket_path)
    fs.atomic_write_text(socket_path, "keep")
    listener.close!()
    check(fs.read_text(socket_path) == "keep", "socket close preserves replacement")
    net.unix.listen(root + "/scoped.sock") |listener|: check(listener.local_path == root + "/scoped.sock", "scoped Unix listener")
    check(not fs.exists?(root + "/scoped.sock"), "Unix listener cleanup")
    caught = false
    try: net.unix.connect("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")
    rescue ArgumentError: caught = true
    check(caught, "Unix path length validation")
    print "PASS remote IO APIs"
  ensure:
    fs.entries(root).each: fs.remove($it)
    fs.remove(root)
  0
