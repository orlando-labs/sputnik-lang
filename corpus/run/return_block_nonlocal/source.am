package conformance.run

def return_from_each():
  xs = [0, 7]
  xs.map |x|:
    if x == 0:
      return "zero"
    "nonzero"
  "missed"

def lambda_return_is_local():
  fn = |x|:
    if x == 0:
      return "lambda-zero"
    "lambda-nonzero"
  fn(0) + ":" + fn(1)

def return_runs_ensure(log):
  try:
    [1].each |x|:
      return x * 40
    -1
  ensure:
    log.push!("ensure")

saved = [null]

def save_block(&blk):
  saved[0] = blk

def create_escaped_block():
  save_block:
    return "wrong"
  "created"

def escaped_block_faults():
  create_escaped_block()
  try:
    saved[0]()
    "missed"
  rescue LocalJumpError:
    "local-jump"

def recursive_return(n):
  if n == 0:
    [1].each |x|:
      return x * 10
  child = recursive_return(n - 1)
  [1].each |x|:
    return child + n
  -1

def probe():
  log = []
  ensured = return_runs_ensure(log)
  "#{return_from_each()}:#{lambda_return_is_local()}:#{ensured}:#{log[0]}:#{escaped_block_faults()}:#{recursive_return(3)}"

def main():
  probe()
