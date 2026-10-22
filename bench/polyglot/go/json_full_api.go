package main
import (
    "bufio"
    "encoding/json"
    "fmt"
    "os"
    "reflect"
)

func parse(text []byte) map[string]any {
    var result map[string]any
    if err := json.Unmarshal(text, &result); err != nil { panic(err) }
    return result
}
func num(m map[string]any, key string) int64 { return int64(m[key].(float64)) }
func mainWorkload() map[string]int64 {
    var checksum, documents, prettyCount, rows int64
    for i := int64(0); i < 1000; i++ {
        document := map[string]any{"id":float64(i),"value":float64(i*2),"group":float64(i%7),
            "items":[]any{float64(i),float64(i+1),float64(i+2)},"name":"event"}
        compact, err := json.Marshal(document); if err != nil { panic(err) }
        parsed := parse(compact)
        if !reflect.DeepEqual(parsed, document) { panic("document mismatch") }; documents++
        checksum += num(parsed,"id") + num(parsed,"value") + num(parsed,"group") + int64(parsed["items"].([]any)[1].(float64))
        if i < 20 {
            pretty, err := json.MarshalIndent(parsed,"","  "); if err != nil { panic(err) }
            reparsed := parse(pretty)
            if !reflect.DeepEqual(reparsed, document) { panic("pretty mismatch") }; prettyCount++
            checksum += int64(reparsed["items"].([]any)[2].(float64))
        }
    }
    file, err := os.Open("bench/polyglot/build/json/events.jsonl"); if err != nil { panic(err) }; defer file.Close()
    scanner := bufio.NewScanner(file)
    for scanner.Scan() {
        row := parse(scanner.Bytes())
        if len(row)!=4 || num(row,"id")!=rows || num(row,"value")!=rows*17%100000 || num(row,"group")!=rows%8 || row["name"]!=fmt.Sprintf("event-%d",rows%100) { panic("row mismatch") }
        checksum += num(row,"id")*3 + num(row,"value") - num(row,"group"); rows++
    }
    if err := scanner.Err(); err != nil { panic(err) }
    return map[string]int64{"checksum":checksum+rows,"documents":documents,"pretty":prettyCount,"rows":rows}
}
func main() {
    r := mainWorkload(); text, err := json.Marshal(r); if err != nil { panic(err) }
    fmt.Println("BENCH_WORK " + string(text)); fmt.Println(r["checksum"])
}
