require "json"
JSON_PATH = "bench/polyglot/build/json/events.jsonl"

def main
  checksum = documents = pretty_count = rows = 0
  1000.times do |i|
    document = {"id" => i, "value" => i * 2, "group" => i % 7,
                "items" => [i, i + 1, i + 2], "name" => "event"}
    parsed = JSON.parse(JSON.generate(document))
    raise "document mismatch" unless parsed == document && parsed.length == 5
    documents += 1
    checksum += parsed["id"] + parsed["value"] + parsed["group"] + parsed["items"][1]
    if i < 20
      pretty = JSON.pretty_generate(parsed)
      raise "not pretty" unless pretty.include?("\n")
      reparsed = JSON.parse(pretty)
      raise "pretty mismatch" unless reparsed == document
      pretty_count += 1
      checksum += reparsed["items"][2]
    end
  end
  File.foreach(JSON_PATH) do |line|
    row = JSON.parse(line)
    raise "row mismatch" unless row.length == 4 && row["id"] == rows && row["value"] == rows * 17 % 100000 && row["group"] == rows % 8 && row["name"] == "event-#{rows % 100}"
    checksum += row["id"] * 3 + row["value"] - row["group"]
    rows += 1
  end
  {"checksum" => checksum + rows, "documents" => documents,
   "pretty" => pretty_count, "rows" => rows}
end

# benchmark entry
result = main
puts "BENCH_WORK #{JSON.generate(result)}"
puts result["checksum"]
