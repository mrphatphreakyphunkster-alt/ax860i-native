# SPDX-License-Identifier: GPL-2.0-only
"""Primary USB offline tests. No device opens, modules or controls."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class NativeUSBTests(unittest.TestCase):
    def compile_and_run(self, filename, expected, pthread=False):
        cc = shutil.which("gcc-13") or shutil.which("cc")
        self.assertIsNotNone(cc, "a C compiler is required")
        with tempfile.TemporaryDirectory(prefix="ax860i-usb-test-") as directory:
            executable = Path(directory) / "test"
            command = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=undefined", "-fno-sanitize-recover=all",
                       "-I", str(ROOT), str(ROOT / "tests" / filename),
                       "-o", str(executable)]
            if pthread:
                command.append("-pthread")
            subprocess.run(command, check=True)
            result = subprocess.run([str(executable)], check=True,
                                    text=True, capture_output=True)
            self.assertIn(expected, result.stdout)
            print(result.stdout.strip())

    def test_shared_codec_conversion_publication_and_tx(self):
        self.compile_and_run("core_test.c", "196608 LINEAR11")

    def test_usb_request_plan_and_allowlist(self):
        self.compile_and_run("usb_plan_test.c", "eight-step UART plan")

    def test_five_page_snapshot_plan_and_fail_closed_total(self):
        self.compile_and_run("snapshot_test.c", "five-page snapshot plan")

    def test_compiled_actual_channel_mapping_and_visibility(self):
        # Compile the actual production functions with only hwmon type stubs;
        # no duplicate mapping or visibility implementation in the harness.
        source = (ROOT / "ax860i_sensors.h").read_text()
        functions = source.split("static int ax_channel_index", 1)[1].split(
            "static int ax_hwmon_read", 1)[0]
        code = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
typedef unsigned int u32;
typedef unsigned short umode_t;
enum hwmon_sensor_types {hwmon_in, hwmon_curr, hwmon_power, hwmon_temp, hwmon_fan};
enum {hwmon_in_label, hwmon_in_input, hwmon_curr_label, hwmon_curr_input,
      hwmon_power_label, hwmon_power_input, hwmon_temp_label, hwmon_temp_input,
      hwmon_fan_label, hwmon_fan_input};
#define AX_TOTAL_POWER_INDEX 14
''' + "static int ax_channel_index" + functions + r'''
int main(void) {
    int ch, labels;
    unsigned int attrs[][2] = {{hwmon_in_input, hwmon_in_label},
        {hwmon_curr_input, hwmon_curr_label}, {hwmon_power_input, hwmon_power_label}};
    for (labels = 0; labels < 2; labels++) {
        assert(!ax_visible(NULL, hwmon_curr, attrs[1][labels], 0));
        for (ch = 1; ch < 4; ch++) {
            assert(ax_visible(NULL, hwmon_curr, attrs[1][labels], ch) == 0444);
            assert(ax_channel_index(hwmon_curr, attrs[1][labels], ch, labels)
                   == 6 + (ch - 1) * 3);
        }
        for (ch = 0; ch < 5; ch++) {
            int expected = ch == 4 ? 14 : (ch ? 7 + (ch - 1) * 3 : 2);
            assert(ax_visible(NULL, hwmon_power, attrs[2][labels], ch) == 0444);
            assert(ax_channel_index(hwmon_power, attrs[2][labels], ch, labels) == expected);
        }
        assert(!ax_visible(NULL, hwmon_power, attrs[2][labels], 5));
        for (ch = 0; ch < 4; ch++)
            assert(ax_channel_index(hwmon_in, attrs[0][labels], ch, labels)
                   == (ch ? 5 + (ch - 1) * 3 : 0));
    }
    return 0;
}
'''
        cc = shutil.which("gcc-13") or shutil.which("cc")
        with tempfile.TemporaryDirectory(prefix="ax860i-surface-") as directory:
            path = Path(directory) / "surface.c"
            path.write_text(code)
            exe = Path(directory) / "surface"
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-fsanitize=undefined",
                            str(path), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
        self.assertIn('"Total DC output"', source)

    def test_abstract_callback_registration_stop_races(self):
        self.compile_and_run("lifecycle_test.c", "100 abstract", pthread=True)

    def test_driver_drain_order_and_stock_api_structure(self):
        # Static regression guard, not a substitute for live lockdep/KASAN.
        source = (ROOT / "ax860i_native.c").read_text()
        stop = source.split("static void ax_quiesce(", 1)[1].split(
            "static void ax_free(", 1)[0]
        sequence = ["ax_stop(&a->core)", "usb_poison_urb(a->rx_urb)",
                    "usb_poison_urb(a->tx_urb)",
                    "cancel_delayed_work_sync(&a->work)",
                    "hwmon_device_unregister(a->hwmon)"]
        positions = [stop.index(item) for item in sequence]
        self.assertEqual(positions, sorted(positions))
        self.assertNotIn("tty_register_ldisc", source)
        self.assertNotIn("usb_bulk_msg(", source)
        self.assertNotIn("usb_control_msg(", source)
        self.assertNotIn("usb_unpoison_urb(", source)
        self.assertIn(".no_dynamic_id = 1", source)
        self.assertIn(".supports_autosuspend = 0", source)


if __name__ == "__main__":
    unittest.main()
