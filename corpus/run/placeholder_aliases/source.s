package conformance.run

def invoke(&blk):
  blk(2, 3)

def probe():
  mapped = [1, 2, 3].map: $it * 2
  combined = invoke(): $it1 * 10 + $it2 + mapped[2]
  mixed = [1, 2].map: $it + $it1 + _1
  nested = [[1, 2], [3, 4]].map:
    outer = $it
    inner = outer.map: $it * 10
    $it[0] + inner[1]
  indexed = [[7], []].map: $it[?0]
  compared = [0, 1].map: $it!=0
  interpolated = [1, 2].map: "value=#{$it}"
  [combined, mixed, nested, indexed, compared, interpolated]

def main():
  print(probe())
