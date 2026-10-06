from sync import Channel

def main():
  channel = Channel.new(capacity: 1)
  producer = task.spawn:
    task.sleep(0.005)
    channel.send(42, timeout: 0.5)

  value = channel.recv(timeout: 0.5)
  sent = producer.wait()
  was_open = not channel.closed?()
  first_close = channel.close()
  second_close = channel.close()
  ok = value == 42 and sent and was_open and first_close
  ok = ok and not second_close and channel.closed?()
  if ok:
    42
  else:
    -1
