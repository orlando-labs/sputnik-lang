import unittest
from run_benchmark import WORKLOADS, aggregate, extract_record, validate_sample


class BenchmarkAdmissionTests(unittest.TestCase):
    def sample(self, stdout):
        return {"stdout": stdout, "stderr": "", "returncode": 0, "elapsed_s": 1.0, "peak_rss_mb": 2.0}

    def test_checksum_cannot_admit_missing_full_json_work(self):
        with self.assertRaises(RuntimeError):
            validate_sample(self.sample("1531352227\n"), WORKLOADS["json-full-api"], "process", 5)

    def test_missing_pretty_roundtrip_rejected_even_with_correct_checksum(self):
        output = 'BENCH_WORK {"checksum":1531352227,"documents":1000,"pretty":0,"rows":20000}\n1531352227\n'
        with self.assertRaises(RuntimeError):
            validate_sample(self.sample(output), WORKLOADS["json-full-api"], "process", 5)

    def test_duplicate_metadata_rejected(self):
        with self.assertRaises(RuntimeError): extract_record('BENCH_BODY {}\nBENCH_BODY {}\n', 'BENCH_BODY ')

    def test_body_batch_normalizes_and_retains_process_wall(self):
        sample = self.sample('BENCH_BODY {"elapsed_ns":100000000,"iterations":5,"warmups":1}\n715609516598740\n')
        validate_sample(sample, WORKLOADS["arithmetic"], "body", 5)
        result = aggregate("cpp", ["cpp"], [sample], "715609516598740")
        self.assertEqual(1.0, result["median_s"])
        self.assertEqual(0.02, result["body_timing_s"]["median"])
        self.assertEqual(0.9, result["outside_body_s"]["median"])

    def test_body_wrong_iteration_count_rejected(self):
        sample = self.sample('BENCH_BODY {"elapsed_ns":100,"iterations":1,"warmups":1}\n715609516598740\n')
        with self.assertRaises(RuntimeError): validate_sample(sample, WORKLOADS["arithmetic"], "body", 5)


if __name__ == "__main__": unittest.main()
