package native.cycle_checkpoint_loop
export main

# No short-lived user method scopes: both loops need their own safepoints to
# retire anchors. Keeping the last cycle live also exercises the frame roots.
def main():
  worker = task.spawn:
    i = 0
    last = null
    while i < 150000:
      cycle = [i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i,
               i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i,
               i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i,
               i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i]
      cycle.push!(cycle)
      last = cycle
      i += 1
    until i == 300000:
      cycle = [i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i,
               i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i,
               i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i,
               i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i]
      cycle.push!(cycle)
      last = cycle
      i += 1
    raise ValueError("live GC root lost") if last[0] != 299999 or last[64][0] != 299999
    i
  worker.wait()
