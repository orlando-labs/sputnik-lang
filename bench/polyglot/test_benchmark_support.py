#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

from benchmark_support import balanced_orders, file_provenance, summary_stats


class BenchmarkSupportTests(unittest.TestCase):
    def test_balanced_orders_rotate_every_program_through_positions(self):
        orders = balanced_orders(["sputnik", "go", "rust"], 3, 0)
        self.assertEqual(3, len(orders))
        self.assertEqual({"sputnik", "go", "rust"}, set(orders[0]))
        for name in ("sputnik", "go", "rust"):
            self.assertEqual({0, 1, 2}, {order.index(name) for order in orders})

    def test_two_program_order_alternates(self):
        orders = balanced_orders(["sputnik", "rails"], 4, 0)
        self.assertEqual(orders[0], orders[2])
        self.assertEqual(orders[1], orders[3])
        self.assertNotEqual(orders[0], orders[1])

    def test_summary_includes_dispersion_and_confidence_interval(self):
        stats = summary_stats([1.0, 2.0, 3.0])
        self.assertEqual(3, stats["count"])
        self.assertEqual(2.0, stats["mean"])
        self.assertEqual(2.0, stats["median"])
        self.assertEqual(1.0, stats["stdev"])
        self.assertLess(stats["ci95_mean_low"], stats["mean"])
        self.assertGreater(stats["ci95_mean_high"], stats["mean"])

    def test_file_provenance_hashes_content(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "artifact"
            path.write_bytes(b"sputnik")
            record = file_provenance(path)
            self.assertEqual(5, record["size_bytes"])
            self.assertEqual(64, len(record["sha256"]))


if __name__ == "__main__":
    unittest.main()
