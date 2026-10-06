package native.str_bytes_core

export main

class CaseProbe:
  def downcase():
    "same"

  def upcase():
    "different"

class CharsProbe:
  def chars():
    ["x", "y"]

def main():
  score = 0
  text = "  Héllo  "
  trimmed = text.trim()
  if trimmed.length() == 5:
    score = score + 1
  if trimmed.bytesize() == 6:
    score = score + 1
  if trimmed.downcase().upcase().starts_with?("H"):
    score = score + 1
  if "A".downcase() != "A".upcase() and "z".upcase() != "z".downcase():
    score = score + 1
  if "9".downcase() == "9".upcase() and "é".upcase() == "é".downcase():
    score = score + 1
  probe = CaseProbe()
  if probe.downcase() != probe.upcase():
    score = score + 1
  if "abé".reverse() == "éba":
    score = score + 1
  chars = "Aé!".chars()
  if chars.count() == 3 and chars[1] == "é":
    score = score + 1
  if chars[0] == "A" and chars[2] == "!":
    score = score + 1
  iterated = ""
  "Aé!".chars().each |char|:
    iterated = iterated + char
  if iterated == "Aé!":
    score = score + 1
  custom_iterated = ""
  CharsProbe().chars().each |char|:
    custom_iterated = custom_iterated + char
  if custom_iterated == "xy":
    score = score + 1
  mutable_chars = "ab".chars()
  visited = ""
  mutable_chars.each |char|:
    visited = visited + char
    if char == "a":
      mutable_chars.push!("c")
  if visited == "ab" and mutable_chars == ["a", "b", "c"]:
    score = score + 1
  all_single_chars = "Az9".chars().all? |char|: char.length() == 1
  if all_single_chars:
    score = score + 1
  parts = "a|bé|c".split("|")
  if parts[1] == "bé" and parts.count() == 3:
    score = score + 1
  if "axbxc".replace("x", "-").ends_with?("-c"):
    score = score + 1
  if "héllo".slice(1...5) == "éllo" and "héllo"[0 - 1] == "o":
    score = score + 1
  bytes = "Aé".bytes()
  if bytes.count() == 3 and bytes[-1] == 169:
    score = score + 1
  if bytes.slice(1, 2).hex() == "c3a9":
    score = score + 1
  if Bytes.new("xy").slice(-1).to_str() == "y" and bytes.empty?() == false:
    score = score + 1
  picked = {a: 1, b: 2, c: 3}.slice("b", "c")
  if picked.count() == 2 and picked["c"] == 3:
    score = score + 1
  if "aé:aé".index(":") == 2 and "aé:aé".index("a", 2) == 3 and "aé:aé".rindex(":") == 2 and "sputnik".rindex("z") == null:
    score = score + 1
  if "header-name".contains_only?("abcdefghijklmnopqrstuvwxyz-") and not "bad name".contains_only?("abcdefghijklmnopqrstuvwxyz-"):
    score = score + 1
  if "hé!".contains_only?("!éh") and "".contains_only?("") and not "x".contains_only?(""):
    score = score + 1
  score
