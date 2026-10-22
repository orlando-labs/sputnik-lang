package bench.polyglot.json_full_api
export main

def check_document(parsed, i):
  raise ValueError("document shape") if parsed.length != 5
  raise ValueError("document fields") if parsed[:id] != i or parsed[:value] != i * 2 or parsed[:group] != i % 7 or parsed[:name] != "event"
  items = parsed[:items]
  raise ValueError("document array") if items.length != 3 or items[0] != i or items[1] != i + 1 or items[2] != i + 2

def workload():
  checksum = 0
  documents = 0
  pretty_count = 0
  rows = 0
  i = 0
  while i < 1000:
    document = {id: i, value: i * 2, group: i % 7, items: [i, i + 1, i + 2], name: "event"}
    compact = document.to_json
    parsed = Json.parse(compact)
    check_document(parsed, i)
    documents = documents + 1
    checksum = checksum + parsed[:id] + parsed[:value] + parsed[:group] + parsed[:items][1]
    if i < 20:
      pretty = Json.pretty_generate(parsed)
      raise ValueError("not pretty") if not pretty.contains?("\n")
      reparsed = Json.parse(pretty)
      check_document(reparsed, i)
      pretty_count = pretty_count + 1
      checksum = checksum + reparsed[:items][2]
    i = i + 1
  count = Json.stream_parse_file("bench/polyglot/build/json/events.jsonl", jsonl: true) |row|:
    raise ValueError("row shape") if row.length != 4
    raise ValueError("row fields") if row[:id] != rows or row[:value] != rows * 17 % 100000 or row[:group] != rows % 8 or row[:name] != "event-" + (rows % 100).to_str
    checksum = checksum + row[:id] * 3 + row[:value] - row[:group]
    rows = rows + 1
  raise ValueError("stream count") if count != rows
  {checksum: checksum + rows, documents: documents, pretty: pretty_count, rows: rows}

# benchmark entry
def main():
  result = workload()
  print "BENCH_WORK " + result.to_json
  result[:checksum]

main()
