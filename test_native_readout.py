# SPDX-License-Identifier: GPL-2.0-only
"""Offline fixtures only: no native module, USB or TTY access."""
import contextlib
import errno
import io
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from ax860i import native_readout as n
from ax860i.protocol import displayed_power, efficiency, windows_power_in


class NativeReadoutTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.dev = self.root / "hwmon4"
        self.dev.mkdir()
        for name, value in {"name": "ax860i_native", "in0_input": "230000",
                            "power1_input": "250000000",
                            "power5_input": "200000000"}.items():
            (self.dev / name).write_text(value + "\n")

    def test_actual_units_raw_estimate_and_json(self):
        result = n.readout(self.root)
        self.assertEqual(result["ac_input_voltage_v"], 230)
        self.assertEqual(result["total_dc_output_w"], 200)
        self.assertEqual(result["ac_input_power_raw_w"], 250)
        q = .00006501415 * 200**2 + (.9914832 * 200) * 1.035 + 11.86228
        self.assertAlmostEqual(result["ac_input_power_estimated_w"], q)
        self.assertAlmostEqual(result["estimated_efficiency_percent"], 20000 / q)
        self.assertFalse(result["atomic"])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(n.main(["--json", "--hwmon-root", str(self.root)]), 0)
        self.assertEqual(json.loads(out.getvalue())["status"], "ok")

    def test_curve_voltage_boundary(self):
        for v in (179.999, 180, 180.001):
            a, b, c = ((.00006501415, .9914832, 11.86228) if v >= 180 else
                       (.00007803814, .9856293, 12.32274))
            self.assertAlmostEqual(n.calculate(v, 300, 200)["ac_input_power_estimated_w"],
                                   a * 200**2 + (b * 200) * 1.035 + c)

    def test_ratio_floor_and_ceiling_branch(self):
        self.assertAlmostEqual(windows_power_in(1, 230), 2.5)
        self.assertAlmostEqual(windows_power_in(100000, 230), 250000)
        # The >=.99 branch is unreachable for ordinary nonnegative AX860i
        # polynomial inputs; exercise its shared implementation with a negative
        # synthetic P. The public native calculator deliberately rejects this.
        self.assertAlmostEqual(windows_power_in(-200, 230), -200 / .99)

    def test_zero_and_strict_fallback_threshold(self):
        self.assertEqual(windows_power_in(0, 230), 0)
        r = n.calculate(230, 12, 0)
        self.assertEqual(r["ac_input_power_estimated_w"], 0)
        self.assertEqual(r["efficiency_input_w"], 12)
        self.assertEqual(r["efficiency_input_source"], "raw_fallback")
        self.assertEqual(r["estimated_efficiency_percent"], 0)
        self.assertEqual(n.calculate(230, 0, 0)["estimated_efficiency_percent"], 0)
        for q in (.000999, .001, .001001):
            with patch("ax860i.protocol.windows_power_in", return_value=q):
                self.assertEqual(displayed_power(1, 230, 12), q if q > .001 else 12)
        self.assertEqual(efficiency(1, .000999), 0)
        self.assertEqual(efficiency(1, .001), 100000)
        self.assertAlmostEqual(efficiency(1, .001001), 100 / .001001)

    def test_nonfinite_negative_and_overflow_rejected(self):
        for bad in (math.nan, math.inf, -math.inf, -1, "bad", 10**500):
            for pos in range(3):
                args = [230, 250, 200]
                args[pos] = bad
                with self.subTest(bad=str(bad), pos=pos), self.assertRaises(n.ReadoutError):
                    n.calculate(*args)
        with self.assertRaisesRegex(n.ReadoutError, "curve result"):
            n.calculate(230, 1, 1e308)

    def test_malformed_sysfs(self):
        for value in ("nan", "inf", "1.5", "", "12\n13", "-1", "9" * 20,
                      "9223372036854775808"):
            (self.dev / "power5_input").write_text(value)
            with self.subTest(value=value), self.assertRaises(n.ReadoutError):
                n.readout(self.root)

    def test_old_abi_missing_total_is_explicit_unavailable(self):
        (self.dev / "power5_input").unlink()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(n.main(["--json", "--hwmon-root", str(self.root)]), 1)
        result = json.loads(out.getvalue())
        self.assertIn("updated native driver required", result["error"])
        self.assertIsNone(result["estimated_efficiency_percent"])

    def test_discovery_exact_name_and_ambiguity(self):
        other = self.root / "hwmon9"
        other.mkdir()
        (other / "name").write_text("ax860i_native_extra\n")
        self.assertEqual(n.discover(self.root), self.dev)
        (other / "name").write_text("ax860i_native\n")
        with self.assertRaisesRegex(n.ReadoutError, "found 2"):
            n.discover(self.root)
        (self.dev / "name").write_text("other")
        (other / "name").write_text("other")
        with self.assertRaisesRegex(n.ReadoutError, "found 0"):
            n.discover(self.root)

    def test_stale_eio_no_cached_fallback(self):
        with patch.object(Path, "open", side_effect=OSError(errno.EIO, "stale/poison")):
            with self.assertRaisesRegex(n.ReadoutError, r"\[Errno 5\] stale/poison"):
                n.read_integer(self.dev, "power5_input")

    def test_bounded_coherence_retry_and_exhaustion(self):
        # raw/total, voltage, raw/total per attempt, repeat the entire sample.
        with patch.object(n, "read_integer", side_effect=[250, 200, 230000, 251, 201,
                                                          252, 202, 231000, 252, 202]) as read:
            result = n.readout(self.root)
            self.assertEqual(result["read_attempts"], 2)
            self.assertEqual(result["ac_input_voltage_v"], 231)
            self.assertEqual(read.call_count, 10)
        with patch.object(n, "read_integer", side_effect=[1, 2, 230000, 3, 4] * 3) as read:
            with self.assertRaisesRegex(n.ReadoutError, "all 3 bounded"):
                n.readout(self.root)
            self.assertEqual(read.call_count, 15)

    def test_human_display_omits_current(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(n.main(["--once", "--hwmon-root", str(self.root)]), 0)
        text = out.getvalue()
        self.assertIn("Estimated efficiency:", text)
        self.assertIn("raw device reading", text)
        self.assertNotIn("current", text)


if __name__ == "__main__":
    unittest.main()
