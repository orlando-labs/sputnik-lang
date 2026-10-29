def check(ok, label):
  raise Exception(label) unless ok

class MembershipContainer:
  def init(@values): @calls = 0
  attr calls
  def contains?(value):
    @calls += 1
    @values.contains?(value)

class MembershipValue:
  def init(@value): @seen = []
  attr seen
  def ==(target):
    @seen.push!(target)
    @value == target

class BadMembershipContainer:
  def contains?(value): 1

class BadMembershipEquality:
  def ==(target): Bool

class MembershipOverride:
  def in?(container): true
  def one_of?(*targets): false

class BadMembershipOverride:
  def in?(container): Bool

class MembershipClassifier:
  def classify(value:, choices:):
    when {value: in choices}: 10
    else: 20

def main():
  check(2.in?([1, 2, 3]), "array membership")
  check(2.in?((1, 2)), "tuple membership")
  check(2.in?({1, 2}), "set membership")
  check(:name.in?({name: null}), "map key normalization")
  check(not :name.in?(StrictMap{"name": null}), "strict map keys")
  check("put".in?("sputnik"), "substring membership")
  check(3.in?(1..5) and not 6.in?(1..5), "range bounds")
  check(9.in?(1..) and 4.in?(Range.new(null, 5, inclusive_end: true)), "open ranges")
  check(3.in?((1..9:2)) and not 4.in?((1..9:2)), "range step")
  check(3.in?((5..1:-2)) and not 4.in?((5..1:-2)), "descending range")
  check(null.in?([null]) and false.in?([false]), "falsy receivers")
  check(Int.in?([Int, Str]) and not 5.in?([Int, Str]), "type values use equality")
  check(not 5.one_of?(Int, 1..10), "one_of does not match types or ranges")
  check(not 2.one_of?() and 2.one_of?(2) and 2.one_of?(1, 2, 3), "variadic arity")
  check(not 2.one_of?([1, 2]), "one collection is one target")
  check([1, 2].one_of?([1, 2]), "array target")
  check([1, 2].one_of?(*[[1, 2]]), "singleton array spread")
  check(2.one_of?(*(1, 2)), "tuple spread")
  check(null.one_of?(false, null) and false.one_of?(null, false), "falsy alternatives")
  value = MembershipValue(2)
  check(value.one_of?(1, 2, 3) and value.seen == [1, 2], "receiver equality and short circuit")
  check(value.in?([value]), "user receiver fallback")
  check(MembershipValue.one_of?(MembershipValue), "class receiver fallback")
  overridden = MembershipOverride()
  check(overridden.in?([]) and not overridden.one_of?(overridden), "method overrides")
  container = MembershipContainer([409, 410])
  calls = []
  make_container = ||:
    calls.push!(:container)
    container
  subject = ||:
    calls.push!(:subject)
    409
  check(subject().in?(make_container()), "evaluation order call")
  check(calls == [:subject, :container] and container.calls == 1, "evaluate once left to right")
  calls.clear!()
  later_target = ||:
    calls.push!(:later)
    999
  check(409.one_of?(409, later_target()) and calls == [:later], "argument evaluation precedes equality short circuit")
  calls.clear!()
  matched = case subject():
    when 0: -1
    when code as in make_container() if code == 409: code
    when in make_container(): -2
    else: -3
  check(matched == 409 and container.calls == 2, "case as and guard")
  check(calls == [:subject, :container], "lazy arm expression")
  nested = case {status: 410, code: "busy"}:
    when {status: in container, code: in ["busy", "wait"]}: true
    else: false
  check(nested, "nested map patterns")
  choices = [1, 2]
  classifier = MembershipClassifier()
  check(classifier.classify(value: 2, choices: choices) == 10 and classifier.classify(value: 3, choices: choices) == 20, "clause pattern")
  alternative = case (9, 2):
    when (in choices | in [9], n as in choices): n
    else: 0
  check(alternative == 2, "nested tuple and OR")
  [n as in choices, tail] = [2, 7]
  check(n == 2 and tail == 7, "pattern assignment")
  selected = [1, 2].map |item as in choices|: item * 2
  check(selected == [2, 4], "block pattern capture")
  choices.push!(3)
  live_match = case 3:
    when in choices: true
    else: false
  check(live_match, "live mutable container")
  try:
    1.in?(null)
    raise Exception("invalid container accepted")
  rescue TypeError:
    null
  try:
    1.in?()
    raise Exception("missing container accepted")
  rescue TypeError:
    null
  try:
    1.in?([1], [2])
    raise Exception("extra container accepted")
  rescue TypeError:
    null
  try:
    1.one_of?(choice: 1)
    raise Exception("one_of keywords accepted")
  rescue TypeError:
    null
  try:
    1.in?([1]): true
    raise Exception("in block accepted")
  rescue TypeError:
    null
  try:
    1.one_of?(1): true
    raise Exception("one_of block accepted")
  rescue TypeError:
    null
  try:
    1.in?(BadMembershipContainer())
    raise Exception("non-Bool contains accepted")
  rescue TypeError:
    null
  try:
    BadMembershipEquality().one_of?(1)
    raise Exception("non-Bool equality accepted")
  rescue TypeError:
    null
  try:
    case BadMembershipOverride():
      when in []: null
    raise Exception("non-Bool pattern result accepted")
  rescue TypeError:
    null
  try:
    [n as in [99], tail] = [2, 8]
    raise Exception("assignment mismatch accepted")
  rescue MatchError:
    check(n == 2 and tail == 7, "failed assignment preserves bindings")
  try:
    case! 99:
      when in choices: null
    raise Exception("case! mismatch accepted")
  rescue MatchError:
    null
  try:
    [99].map |item as in choices|: item
    raise Exception("block mismatch accepted")
  rescue MatchError:
    null
  0
