# SPDX-License-Identifier: GPL-2.0-only
"""Read native hwmon only; never open a TTY, USB node or transport."""
import argparse
import json
import math
from pathlib import Path
import re
import sys

from .protocol import displayed_power, efficiency, windows_power_in


class ReadoutError(Exception):
    """Unavailable, malformed or changing native telemetry."""


FIELDS = ("ac_input_voltage_v", "ac_input_power_raw_w",
          "ac_input_power_estimated_w", "total_dc_output_w",
          "estimated_efficiency_percent")
METHOD = "Corsair LINK 4.9.9.3 AX860i curve estimate from native LINEAR11 values"


def discover(root=Path("/sys/class/hwmon")):
    """Require exactly one exact-name device; never guess by hwmon number."""
    matches = []
    try:
        for path in sorted(Path(root).glob("hwmon*")):
            try:
                name = (path / "name").read_text(encoding="ascii").strip()
            except FileNotFoundError:
                continue  # May have disappeared during enumeration.
            if name == "ax860i_native":
                matches.append(path)
    except (OSError, UnicodeError) as exc:
        raise ReadoutError(f"hwmon discovery failed: {exc}") from exc
    if len(matches) != 1:
        raise ReadoutError(f"expected one ax860i_native device, found {len(matches)}")
    return matches[0]


def read_integer(device, attribute):
    try:
        with (device / attribute).open(encoding="ascii") as stream:
            text = stream.read(128).strip()
    except FileNotFoundError as exc:
        if attribute == "power5_input":
            raise ReadoutError("power5_input unavailable: updated native driver required; "
                               "reload needs approval (no three-rail subtotal fallback)") from exc
        raise ReadoutError(f"{attribute} unavailable: {exc}") from exc
    except (OSError, UnicodeError) as exc:
        raise ReadoutError(f"{attribute} unavailable: {exc}") from exc
    if not re.fullmatch(r"-?[0-9]{1,19}", text):
        raise ReadoutError(f"malformed {attribute}: expected integer hwmon units")
    value = int(text)
    if not 0 <= value <= (1 << 63) - 1:
        raise ReadoutError(f"{attribute}: negative or out-of-range reading")
    return value


def calculate(input_v, measured_w, output_w):
    for name, value in (("input voltage", input_v), ("raw input power", measured_w),
                        ("total output power", output_w)):
        try:
            valid = math.isfinite(value) and value >= 0
        except (TypeError, OverflowError):
            valid = False
        if not valid:
            raise ReadoutError(f"{name} must be finite and nonnegative")
    estimate = windows_power_in(output_w, input_v)
    denominator = displayed_power(output_w, input_v, measured_w)
    percent = efficiency(output_w, denominator)
    if not all(math.isfinite(v) and v >= 0 for v in (estimate, denominator, percent)):
        raise ReadoutError("nonfinite or negative curve result")
    return dict(ac_input_voltage_v=input_v, ac_input_power_raw_w=measured_w,
                ac_input_power_estimated_w=estimate, total_dc_output_w=output_w,
                estimated_efficiency_percent=percent,
                efficiency_input_w=denominator,
                efficiency_input_source="curve" if abs(estimate) > .001 else "raw_fallback")


def readout(root=Path("/sys/class/hwmon"), attempts=3):
    if not 1 <= attempts <= 10:
        raise ValueError("attempts must be in 1..10")
    device = discover(root)
    for attempt in range(1, attempts + 1):
        before = tuple(read_integer(device, name) for name in
                       ("power1_input", "power5_input"))
        voltage = read_integer(device, "in0_input")
        after = tuple(read_integer(device, name) for name in
                      ("power1_input", "power5_input"))
        if before == after:
            return dict(status="ok", hwmon=str(device), method=METHOD,
                        atomic=False, coherence="equal power values before/after; not atomic",
                        read_attempts=attempt,
                        **calculate(voltage / 1000, after[0] / 1000000,
                                    after[1] / 1000000))
    raise ReadoutError(f"power values changed across all {attempts} bounded read attempts")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="machine-readable one-shot output")
    parser.add_argument("--once", action="store_true", help="one snapshot (the default)")
    parser.add_argument("--hwmon-root", type=Path, default=Path("/sys/class/hwmon"),
                        help="hwmon class root; override for offline fixtures")
    args = parser.parse_args(argv)
    try:
        result = readout(args.hwmon_root)
    except ReadoutError as exc:
        result = dict.fromkeys(FIELDS)
        result.update(status="unavailable", error=str(exc), atomic=False, method=METHOD)
    if args.json:
        print(json.dumps(result, allow_nan=False, sort_keys=True))
    elif result["status"] == "ok":
        for key, label, unit in (
                (FIELDS[0], "AC input voltage", "V"),
                (FIELDS[1], "AC input power (raw device reading)", "W"),
                (FIELDS[2], "AC input power (Corsair curve estimate)", "W"),
                (FIELDS[3], "Total DC output (five measured page powers)", "W"),
                (FIELDS[4], "Estimated efficiency", "%")):
            print(f"{label}: {result[key]:.3f} {unit}")
        print("Sequential hwmon reads, not atomic; estimate is not measured efficiency.")
        if result["efficiency_input_source"] == "raw_fallback":
            print("Near-zero curve: efficiency denominator uses raw input-power fallback.")
    else:
        print(f"Native readout unavailable: {result['error']}", file=sys.stderr)
    return 0 if result["status"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
