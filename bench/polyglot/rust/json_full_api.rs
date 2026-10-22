use serde_json::{json, Value};
use std::fs::File;
use std::io::{BufRead, BufReader};

fn main_workload() -> Value {
    let (mut checksum, mut documents, mut pretty_count, mut rows) = (0i64, 0i64, 0i64, 0i64);
    for i in 0..1000i64 {
        let document = json!({"id":i, "value":i*2, "group":i%7, "items":[i,i+1,i+2], "name":"event"});
        let compact = serde_json::to_string(&document).unwrap();
        let parsed: Value = serde_json::from_str(&compact).unwrap();
        assert_eq!(parsed, document); documents += 1;
        checksum += parsed["id"].as_i64().unwrap() + parsed["value"].as_i64().unwrap() +
                    parsed["group"].as_i64().unwrap() + parsed["items"][1].as_i64().unwrap();
        if i < 20 {
            let pretty = serde_json::to_string_pretty(&parsed).unwrap();
            assert!(pretty.contains('\n'));
            let reparsed: Value = serde_json::from_str(&pretty).unwrap();
            assert_eq!(reparsed, document); pretty_count += 1;
            checksum += reparsed["items"][2].as_i64().unwrap();
        }
    }
    for line in BufReader::new(File::open("bench/polyglot/build/json/events.jsonl").unwrap()).lines() {
        let row: Value = serde_json::from_str(&line.unwrap()).unwrap();
        assert_eq!(row.as_object().unwrap().len(), 4);
        assert_eq!(row["id"].as_i64().unwrap(), rows);
        assert_eq!(row["value"].as_i64().unwrap(), rows*17%100000);
        assert_eq!(row["group"].as_i64().unwrap(), rows%8);
        assert_eq!(row["name"].as_str().unwrap(), format!("event-{}", rows%100));
        checksum += row["id"].as_i64().unwrap()*3 + row["value"].as_i64().unwrap() - row["group"].as_i64().unwrap();
        rows += 1;
    }
    json!({"checksum":checksum+rows,"documents":documents,"pretty":pretty_count,"rows":rows})
}

fn main() {
    let r = main_workload();
    println!("BENCH_WORK {}", r);
    println!("{}", r["checksum"].as_i64().unwrap());
}
