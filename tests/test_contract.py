#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Shravya Juluru
"""
End-to-end check that the VCD path honours CONTRACT.md.

Runs `wfm_query skills/examples/fixtures/counter.vcd <cmd>` for real and asserts
the guaranteed behaviour: exit codes (0 / 1 / 2), the `Resolved:` stderr line,
the `<binary> (0x<hex>)` value format, ns timestamp formatting, and stable
headers/totals. This is the only backend that can run in CI - FSDB needs a
Verdi license and SHM needs Xcelium - but the contract it verifies is the same
one all three backends implement.

    python3 tests/test_contract.py          # or: python -m unittest -v tests.test_contract
"""

import os
import re
import subprocess
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
WFM = os.path.join(ROOT, "wfm_query")
FIXTURE = os.path.join(ROOT, "skills", "examples", "fixtures", "counter.vcd")


def run(*args, stdin=None):
    p = subprocess.run([sys.executable, WFM, FIXTURE, *args],
                       capture_output=True, text=True, input=stdin)
    return p.returncode, p.stdout, p.stderr


class ExitCodes(unittest.TestCase):
    def test_ok_is_zero(self):
        rc, out, _ = run("info")
        self.assertEqual(rc, 0)
        self.assertIn("Signals:", out)

    def test_not_found_is_one(self):
        rc, _, err = run("value", "no_such_signal", "10")
        self.assertEqual(rc, 1)

    def test_multi_match_is_one_with_list(self):
        rc, _, err = run("value", "clk", "10")   # tb.clk and tb.dut.clk
        self.assertEqual(rc, 1)
        self.assertIn("Multiple signals match", err)
        self.assertRegex(err, r"\n  1\) ")

    def test_usage_error_is_two(self):
        rc, _, _ = run("value")                  # missing operands
        self.assertEqual(rc, 2)

    def test_unknown_command_is_two(self):
        rc, _, _ = run("wiggle")
        self.assertEqual(rc, 2)


class StderrConventions(unittest.TestCase):
    def test_resolved_line_present_and_exact(self):
        rc, _, err = run("value", "tb.dut.count", "60")
        self.assertEqual(rc, 0)
        self.assertIn("Resolved: tb.dut.count\n", err)

    def test_slash_path_normalises(self):
        rc, out, err = run("value", "/tb/dut/count", "60")
        self.assertEqual(rc, 0)
        self.assertIn("Resolved: tb.dut.count\n", err)


class ValueFormatting(unittest.TestCase):
    def test_value_binary_hex_shape(self):
        rc, out, _ = run("value", "tb.dut.count", "60")
        self.assertEqual(rc, 0)
        m = re.search(r"^Value:\s+([01xz]+) \(0x([0-9a-fx]+)\)$", out, re.M)
        self.assertIsNotNone(m, out)
        self.assertEqual(m.group(1), "00000100")     # counter == 4 at 60 ns
        self.assertEqual(m.group(2), "04")

    def test_timestamp_decimal_ns_rule(self):
        # >= 1 ns -> 3 places
        rc, out, _ = run("value", "tb.dut.count", "60")
        self.assertRegex(out, r"(?m)^Time:\s+60\.000 ns$")

    def test_changes_headers_and_total(self):
        rc, out, _ = run("changes", "tb.dut.count", "40", "120")
        self.assertEqual(rc, 0)
        self.assertRegex(out, r"(?m)^Time\s+Binary\s+Hex$")
        self.assertRegex(out, r"\nTotal: \d+ value changes\s*$")
        for line in out.splitlines():
            m = re.match(r"^(\d+\.\d{3})\s+[01xz]+\s+0x[0-9a-fx]+$", line)
            if m:
                break
        else:
            self.fail("no data row matched the guaranteed shape:\n" + out)


class Commands(unittest.TestCase):
    def test_scopes_total_line(self):
        rc, out, _ = run("scopes", "dut")
        self.assertEqual(rc, 0)
        self.assertRegex(out, r"\nTotal: \d+ scopes\s*$")

    def test_signals_total_line(self):
        rc, out, _ = run("signals", "count")
        self.assertEqual(rc, 0)
        self.assertRegex(out, r"\nTotal matches: \d+\s*$")

    def test_freq_measures_the_clock(self):
        rc, out, _ = run("freq", "dut")
        self.assertEqual(rc, 0)
        self.assertRegex(out, r"clk\s+100\.0 MHz\s+\(period 10\.000 ns\)")

    def test_debug_snapshot_and_transitions(self):
        rc, out, _ = run("debug", "dut", "50", "-w", "20")
        self.assertEqual(rc, 0)
        self.assertIn("=== dut @ 50.000 ns", out)
        self.assertIn("--- transitions in window ---", out)

    def test_batch_echoes_and_runs(self):
        rc, out, _ = run("batch", stdin="info\nfreq dut\n")
        self.assertEqual(rc, 0)
        self.assertIn(">>> info", out)
        self.assertIn(">>> freq dut", out)


if __name__ == "__main__":
    if not os.path.exists(FIXTURE):
        sys.exit("fixture missing: %s (run skills/examples/fixtures/gen_counter_vcd.py)" % FIXTURE)
    unittest.main(verbosity=2)
