"""Generate explicit body-timer entries without changing process-wall fixtures.

Only fixtures with a callable, checksum-returning body are admitted. Generated
sources are included in provenance. One in-process warmup precedes the timed
batch; every result remains observable, preventing C++/Rust CSE across calls.
"""
from pathlib import Path


def prepare(root: Path, build: Path, workload, iterations: int) -> dict:
    if workload.name not in ("arithmetic", "json-full-api"):
        raise ValueError("body timing is supported for arithmetic and json-full-api")
    out = build / "body-sources"
    out.mkdir(parents=True, exist_ok=True)
    full = workload.name == "json-full-api"
    paths = {}
    for language, filename in (("sputnik", workload.sputnik_source), ("python", workload.python_source),
                               ("ruby", workload.ruby_source), ("cpp", workload.cpp_source),
                               ("go", workload.go_source), ("rust", workload.rust_source)):
        base = root / "bench/polyglot" / language
        if language == "sputnik": base /= "src"
        text = (base / filename).read_text()
        if language == "sputnik":
            if full: text = text.split("# benchmark entry\n")[0]
            else:
                assert text.endswith("main()\n")
                text = text[:-len("main()\n")].replace("def main():", "def workload():", 1)
            checksum = "result[:checksum]" if full else "result"
            text += f'''\ndef main():
  result = workload()
  started = Time.monotonic
  i = 0
  while i < {iterations}:
    result = workload()
    raise ValueError("body checksum") if {checksum} != {workload.expected_checksum}
    i = i + 1
  elapsed = (Time.monotonic - started).total_nanoseconds
  print "BENCH_BODY " + {{elapsed_ns: elapsed, iterations: {iterations}, warmups: 1}}.to_json
'''
            if full: text += '  print "BENCH_WORK " + result.to_json\n'
            text += f"  {checksum}\n\nmain()\n"
        elif language == "python":
            text = text.split('if __name__ == "__main__":')[0]
            checksum = 'result["checksum"]' if full else "result"
            text += f'''\nimport time
import json
result = main()
started = time.perf_counter_ns()
for _ in range({iterations}):
    result = main()
    assert {checksum} == {workload.expected_checksum}
elapsed = time.perf_counter_ns() - started
print("BENCH_BODY " + json.dumps({{"elapsed_ns": elapsed, "iterations": {iterations}, "warmups": 1}}))
'''
            if full: text += 'print("BENCH_WORK " + json.dumps(result))\n'
            text += f"print({checksum})\n"
        elif language == "ruby":
            text = text.split("# benchmark entry\n")[0] if full else text.removesuffix("puts main\n")
            checksum = 'result["checksum"]' if full else "result"
            text += f'''\nrequire "json"
result = main
started = Process.clock_gettime(Process::CLOCK_MONOTONIC, :nanosecond)
{iterations}.times do
  result = main
  raise "body checksum" unless {checksum} == {workload.expected_checksum}
end
elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC, :nanosecond) - started
puts "BENCH_BODY #{{JSON.generate({{elapsed_ns: elapsed, iterations: {iterations}, warmups: 1}})}}"
'''
            if full: text += 'puts "BENCH_WORK #{JSON.generate(result)}"\n'
            text += f"puts {checksum}\n"
        elif language == "cpp":
            text = text.split("int main()")[0]
            text = '#include <chrono>\n#include <cstdlib>\n' + text
            text = text.replace("std::int64_t main_workload()", "__attribute__((noinline)) std::int64_t main_workload()")
            text = text.replace("Result main_workload()", "__attribute__((noinline)) Result main_workload()")
            checksum = "result.checksum" if full else "result"
            # Memory clobber prevents reuse of a pure noinline workload result.
            text += f'''\nint main() {{
  auto workload_fn = &main_workload;
  asm volatile("" : "+r"(workload_fn) : : "memory");
  auto result = workload_fn();
  asm volatile("" : : "g"({checksum}) : "memory");
  const auto started = std::chrono::steady_clock::now();
  for (int n = 0; n < {iterations}; ++n) {{
    asm volatile("" : "+r"(workload_fn) : : "memory");
    result = workload_fn();
    if ({checksum} != {workload.expected_checksum}LL) std::abort();
    asm volatile("" : : "g"({checksum}) : "memory");
  }}
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
  std::cout << "BENCH_BODY {{\\\"elapsed_ns\\\":" << elapsed << ",\\\"iterations\\\":{iterations},\\\"warmups\\\":1}}\\n";
'''
            if full: text += "  print_work(result);\n"
            text += f"  std::cout << {checksum} << '\\n';\n}}\n"
        elif language == "go":
            text = text.split("func main()")[0]
            text = text.replace('import "fmt"', 'import ("fmt"; "time")') if not full else text.replace('"reflect"', '"reflect"\n"time"')
            checksum = 'result["checksum"]' if full else "result"
            text += f'''\nfunc main() {{
  result := mainWorkload()
  started := time.Now()
  for n := 0; n < {iterations}; n++ {{ result = mainWorkload(); if {checksum} != {workload.expected_checksum} {{ panic("body checksum") }} }}
  elapsed := time.Since(started).Nanoseconds()
  fmt.Printf("BENCH_BODY {{\\\"elapsed_ns\\\":%d,\\\"iterations\\\":{iterations},\\\"warmups\\\":1}}\\n", elapsed)
'''
            if full: text += '  text, err := json.Marshal(result); if err != nil { panic(err) }; fmt.Println("BENCH_WORK " + string(text))\n'
            text += f"  fmt.Println({checksum})\n}}\n"
        else:
            text = text.split("fn main()")[0]
            checksum = 'result["checksum"].as_i64().unwrap()' if full else "result"
            result_type = "Value" if full else "i64"
            text += f'''\nfn main() {{
  let workload_fn = main_workload as fn() -> {result_type};
  let mut result = std::hint::black_box(workload_fn)();
  std::hint::black_box(&result);
  let started = std::time::Instant::now();
  for _ in 0..{iterations} {{ result = std::hint::black_box(workload_fn)(); assert_eq!({checksum}, {workload.expected_checksum}); }}
  let elapsed = started.elapsed().as_nanos();
  println!("BENCH_BODY {{{{\\\"elapsed_ns\\\":{{}},\\\"iterations\\\":{iterations},\\\"warmups\\\":1}}}}", elapsed);
'''
            if full: text += '  println!("BENCH_WORK {}", result);\n'
            text += f'  println!("{{}}", {checksum});\n}}\n'
        path = out / filename
        path.write_text(text)
        paths[language] = path
    return paths
