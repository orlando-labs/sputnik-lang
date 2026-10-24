import task

export main

def check(condition, message):
  raise ValueError("BigInt: " + message) unless condition

class Codec:
  def encode(value, now:): value + now
  def decode(value, now:): value - now

def main():
  huge = BigInt("123456789012345678901234567890")
  check(BigInt(huge) == huge, "identity conversion")
  check((huge + 1).str == "123456789012345678901234567891", "addition")
  check((1 + huge).str == "123456789012345678901234567891", "mixed Int receiver")
  check((huge - 1).str == "123456789012345678901234567889", "subtraction")
  check((1 - huge).str == "-123456789012345678901234567889", "mixed subtraction")
  check((huge * 3).str == "370370367037037036703703703670", "multiplication")
  check((huge / 3).str == "41152263004115226300411522630", "division")
  check((BigInt(2) ** 130).str == "1361129467683753853853498429727072845824", "power beyond Int")
  check((2 ** BigInt(70)).str == "1180591620717411303424", "mixed power")
  check((BigInt(2) ** BigInt(10)).to_int == 1024, "BigInt exponent")
  check(BigInt(-7) / 3 == BigInt(-2) and BigInt(-7) // 3 == BigInt(-3), "signed division")
  check(BigInt(-7) % 3 == BigInt(2) and BigInt(7) % -3 == BigInt(-2), "signed modulo")
  check((+huge) == huge and (-huge).abs == huge, "unary operators")
  check(huge > 10 and 10 < huge and huge >= huge and huge <= huge, "comparisons")
  check((huge <=> (huge + 1)) == -1 and (10 <=> huge) == -1, "three-way comparison")
  check(BigInt === huge and not (Int === huge), "runtime type")
  check(not (BigInt === 5), "BigInt matcher does not match Int")
  matched = case huge:
    when BigInt: true
    else: false
  check(matched, "pattern matcher")
  check(BigInt(5) == 5 and not (5 == BigInt(5)) and not (BigInt(5) == 5.0), "existing VM equality contract")
  check(BigInt("+1_000").to_int == 1000 and BigInt("-0").str == "0", "decimal parsing")
  check(Int(BigInt(42)) == 42 and Str(huge) == huge.str, "explicit conversion constructors")
  check(BigInt("9223372036854775807").to_int == 9223372036854775807, "Int maximum")
  check(BigInt("-9223372036854775808").to_int == -9223372036854775807 - 1, "Int minimum")
  check([huge, huge + 1] == [BigInt(huge.str), huge + 1], "value equality in arrays")
  stored = {}
  stored[huge] = huge + 1
  check(stored[BigInt(huge.str)] == huge + 1, "immutable map keys")
  check(Map(stored.entries)[BigInt(huge.str)] == huge + 1, "map conversion preserves BigInt values")
  child = task.spawn: huge * 2
  check(child.wait() == huge + huge, "immutable values cross tasks")
  codec = Codec()
  check(codec.decode(codec.encode(huge, now: 7), now: 7) == huge, "user codec keywords remain native")
  check("ab" * 3 == "ababab" and "ab" * 0 == "", "native string repetition for session limits")
  rejected = false
  try:
    huge.to_int
  rescue OverflowError:
    rejected = true
  check(rejected, "explicit narrowing overflow")
  rejected = false
  try:
    huge / 0
  rescue ZeroDivisionError:
    rejected = true
  check(rejected, "division by zero")
  rejected = false
  try:
    huge % 0
  rescue ZeroDivisionError:
    rejected = true
  check(rejected, "modulo by zero")
  rejected = false
  try:
    9223372036854775807 + 1
  rescue OverflowError:
    rejected = true
  check(rejected, "Int never implicitly promotes")
  print "PASS native BigInt"
  0

main()
