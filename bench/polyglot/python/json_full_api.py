"""Full generic JSON materialization; counts are checked by the runner."""
import json

JSON_PATH = "bench/polyglot/build/json/events.jsonl"


def main():
    checksum = documents = pretty_count = rows = 0
    for i in range(1000):
        document = {"id": i, "value": i * 2, "group": i % 7,
                    "items": [i, i + 1, i + 2], "name": "event"}
        compact = json.dumps(document, separators=(",", ":"))
        parsed = json.loads(compact)
        assert parsed == document and len(parsed) == 5
        documents += 1
        checksum += parsed["id"] + parsed["value"] + parsed["group"] + parsed["items"][1]
        if i < 20:
            pretty = json.dumps(parsed, indent=2)
            assert "\n" in pretty
            reparsed = json.loads(pretty)
            assert reparsed == document
            pretty_count += 1
            checksum += reparsed["items"][2]
    with open(JSON_PATH, encoding="utf-8") as handle:
        for line in handle:
            row = json.loads(line)
            assert len(row) == 4 and row["id"] == rows and row["value"] == rows * 17 % 100000
            assert row["group"] == rows % 8 and row["name"] == f"event-{rows % 100}"
            checksum += row["id"] * 3 + row["value"] - row["group"]
            rows += 1
    return {"checksum": checksum + rows, "documents": documents,
            "pretty": pretty_count, "rows": rows}


if __name__ == "__main__":
    result = main()
    print("BENCH_WORK " + json.dumps(result))
    print(result["checksum"])
