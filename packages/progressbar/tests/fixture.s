package progressbar.tests

from progressbar import ProgressBar, with_progress

export main

def main():
  known = with_progress([1, 2, 3], desc: "known", throttle: 0).each |item|:
    item * 2
  if known != [1, 2, 3] or ProgressBar(total: 4, throttle: 0).inc!(2) != 2:
    raise ValueError("progressbar eager adapter regression")

  unknown = with_progress((1..3).lazy, desc: "unknown", throttle: 0)
  unknown.each |item|:
    item
  0
