# SPDX-License-Identifier: GPL-2.0-only
"""Offline only: fake sysfs, fake subprocesses, and project-local stage trees."""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import types
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parent


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


binder = load("persistence_binder", ROOT / "persistence/ax860i-native-bind")
build = load("persistence_build", ROOT / "native/build_module.py")
package = load("persistence_package", ROOT / "persistence/package.py")


class FakeBinder(binder.Binder):
    def __init__(self, root):
        self.now = 0
        self.events = []
        self.busy = False
        self.fail_probe = False
        self.fail_rollback = False
        self.auto_bind = False
        self.disconnect = False
        self.takeover = False
        self.fail_load = False
        super().__init__(root / "sys", root / "dev", self.run_fake,
                         self.sleep_fake, lambda: self.now)
        self.devices.mkdir(parents=True)
        (self.sysroot / "devices").mkdir()
        self.devroot.mkdir()
        for driver in ("cp210x", binder.NATIVE, "other"):
            (self.drivers / driver).mkdir(parents=True)

    def make_interface(self, name="1-2:1.0", vendor="1b1c", product="1c00", owner="cp210x"):
        parent = self.sysroot / "devices" / name.split(":")[0]
        parent.mkdir(exist_ok=True)
        (parent / "idVendor").write_text(vendor)
        (parent / "idProduct").write_text(product)
        path = parent / name
        path.mkdir()
        for key, value in {"bInterfaceNumber": "00", "bAlternateSetting": "0",
                           "bInterfaceClass": "ff", "bInterfaceSubClass": "00",
                           "bInterfaceProtocol": "00"}.items():
            (path / key).write_text(value)
        interface = self.devices / name
        interface.symlink_to(path)
        tty = path / "ttyUSB0/tty/ttyUSB0"
        tty.mkdir(parents=True)
        (self.devroot / "ttyUSB0").touch()
        self.set_owner(interface, owner)
        return interface

    def set_owner(self, interface, owner):
        link = interface / "driver"
        link.unlink(missing_ok=True)
        if owner:
            link.symlink_to(self.drivers / owner)

    def publish(self, interface):
        hwmon = interface / "hwmon/hwmon0"
        hwmon.mkdir(parents=True, exist_ok=True)
        for name, value in {"name": binder.NATIVE, "power1_input": "100000000",
                            "power5_input": "85000000", "in0_input": "230000"}.items():
            (hwmon / name).write_text(value)

    def sleep_fake(self, seconds):
        self.now += seconds

    def run_fake(self, command, **kwargs):
        self.events.append(tuple(command))
        if command[0] == "/usr/bin/fuser":
            return types.SimpleNamespace(returncode=0 if self.busy else 1, stderr="")
        if command != ["/usr/sbin/modprobe", binder.NATIVE]:
            raise AssertionError(f"unexpected command {command}")
        if self.fail_load:
            raise subprocess.CalledProcessError(1, command)
        for interface in self.devices.iterdir():
            if self.auto_bind and self.identity(interface) and self.owner(interface) is None:
                self.set_owner(interface, binder.NATIVE)
                if not self.fail_probe:
                    self.publish(interface)
        return types.SimpleNamespace(returncode=0)

    def write(self, interface, driver, operation):
        # Exercise the real writer's identity/ownership gate. Its regular-file
        # fake sysfs output is then interpreted as a synchronous kernel action.
        self.events.append((driver, operation, interface.name))
        if self.fail_rollback and driver == "cp210x" and operation == "bind":
            raise OSError("fake rollback failure")
        super().write(interface, driver, operation)
        self.set_owner(interface, None if operation == "unbind" else driver)
        if operation == "bind" and driver == binder.NATIVE:
            if self.disconnect:
                (interface / "bInterfaceNumber").unlink()
            elif self.takeover:
                self.set_owner(interface, "other")
            elif not self.fail_probe:
                self.publish(interface)


class BindingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / "persistence")
        self.addCleanup(self.temp.cleanup)
        self.b = FakeBinder(Path(self.temp.name))

    def test_exact_target_and_no_global_commands(self):
        target = self.b.make_interface()
        self.assertEqual(self.b.attach(target), "native snapshot ready")
        self.assertEqual(self.b.owner(target), binder.NATIVE)
        self.assertEqual([event for event in self.b.events if event[0] in ("cp210x", binder.NATIVE)],
                         [("cp210x", "unbind", target.name),
                          (binder.NATIVE, "bind", target.name)])
        self.assertEqual([event for event in self.b.events if event[0].startswith("/")],
                         [("/usr/bin/fuser", "--", str(self.b.devroot / "ttyUSB0")),
                          ("/usr/sbin/modprobe", binder.NATIVE),
                          ("/usr/bin/fuser", "--", str(self.b.devroot / "ttyUSB0"))])

    def test_identity_driver_and_alternate_mismatches_are_untouched(self):
        for key, value in (("vendor", "ffff"), ("product", "ffff"), ("owner", "other")):
            with self.subTest(key=key):
                interface = self.b.make_interface(name=f"1-{len(list(self.b.devices.iterdir()))+2}:1.0",
                                                  **{key: value})
                self.b.attach(interface)
                self.assertEqual(self.b.events, [])
        interface = self.b.make_interface(name="1-9:1.0")
        for key, bad in (("bInterfaceNumber", "01"), ("bAlternateSetting", "1"),
                         ("bInterfaceClass", "02"), ("bInterfaceSubClass", "01"),
                         ("bInterfaceProtocol", "01")):
            old = (interface / key).read_text()
            (interface / key).write_text(bad)
            self.assertEqual(self.b.attach(interface), "ignored")
            (interface / key).write_text(old)
        self.assertEqual(self.b.events, [])

    def test_already_native_is_idempotent_even_without_snapshot(self):
        interface = self.b.make_interface(owner=binder.NATIVE)
        self.assertIn("already native", self.b.attach(interface))
        self.assertEqual(self.b.events, [])

    def test_busy_tty_never_loads_or_unbinds(self):
        interface = self.b.make_interface()
        self.b.busy = True
        with self.assertRaisesRegex(binder.BindError, "busy"):
            self.b.attach(interface)
        self.assertEqual(self.b.owner(interface), "cp210x")
        self.assertEqual(len(self.b.events), 1)

    def test_missing_tty_fails_closed(self):
        interface = self.b.make_interface()
        (self.b.devroot / "ttyUSB0").unlink()
        with self.assertRaisesRegex(binder.BindError, "check failed"):
            self.b.attach(interface)
        self.assertEqual(self.b.owner(interface), "cp210x")

    def test_failed_probe_restores_cp210x_and_reports_error(self):
        interface = self.b.make_interface()
        self.b.fail_probe = True
        with self.assertRaisesRegex(binder.BindError, "original ownership restored"):
            self.b.attach(interface)
        self.assertEqual(self.b.now, 45)
        self.assertEqual(self.b.owner(interface), "cp210x")
        self.assertEqual(self.b.events[-2:], [(binder.NATIVE, "unbind", interface.name),
                                             ("cp210x", "bind", interface.name)])

    def test_synchronous_bind_failure_restores_cp210x(self):
        interface = self.b.make_interface()
        original = self.b.write

        def fail_native(path, driver, operation):
            if (driver, operation) == (binder.NATIVE, "bind"):
                raise OSError("probe refused")
            return original(path, driver, operation)

        self.b.write = fail_native
        with self.assertRaisesRegex(binder.BindError, "probe refused"):
            self.b.attach(interface)
        self.assertEqual(self.b.owner(interface), "cp210x")

    def test_rollback_failure_is_not_hidden(self):
        interface = self.b.make_interface()
        self.b.fail_probe = self.b.fail_rollback = True
        with self.assertRaisesRegex(binder.BindError, "ROLLBACK FAILED"):
            self.b.attach(interface)

    def test_unbound_autoprobe_is_verified_and_original_none_restored_on_failure(self):
        interface = self.b.make_interface(owner=None)
        self.b.auto_bind = self.b.fail_probe = True
        with self.assertRaisesRegex(binder.BindError, "original ownership restored"):
            self.b.attach(interface)
        self.assertIsNone(self.b.owner(interface))
        self.assertFalse(any(event[0] == "cp210x" for event in self.b.events))

    def test_unbound_autoprobe_success_needs_no_bind_write(self):
        interface = self.b.make_interface(owner=None)
        self.b.auto_bind = True
        self.assertEqual(self.b.attach(interface), "native snapshot ready")
        self.assertEqual(self.b.events, [("/usr/sbin/modprobe", binder.NATIVE)])

    def test_missing_tty_tree_refuses_unbind(self):
        interface = self.b.make_interface()
        (interface / "ttyUSB0/tty/ttyUSB0").rmdir()
        with self.assertRaisesRegex(binder.BindError, "not present yet"):
            self.b.attach(interface)
        self.assertEqual(self.b.events, [])

    def test_driver_and_device_links_outside_allowlist_are_rejected(self):
        interface = self.b.make_interface()
        self.b.set_owner(interface, None)
        (interface / "driver").symlink_to(self.b.devroot)
        with self.assertRaisesRegex(binder.BindError, "outside USB"):
            self.b.attach(interface)
        escaped = self.b.devices / "1-8:1.0"
        escaped.symlink_to(self.b.devroot)
        self.assertFalse(self.b.identity(escaped))
        self.assertEqual(self.b.events, [])

    def test_modprobe_failure_preserves_cp210x(self):
        interface = self.b.make_interface()
        self.b.fail_load = True
        with self.assertRaises(binder.BindError):
            self.b.attach(interface)
        self.assertEqual(self.b.owner(interface), "cp210x")
        self.assertFalse(any(event[0] == "cp210x" for event in self.b.events))

    def test_disconnect_does_not_write_during_rollback(self):
        interface = self.b.make_interface()
        self.b.disconnect = True
        with self.assertRaisesRegex(binder.BindError, "disconnected"):
            self.b.attach(interface)
        self.assertEqual(self.b.events[-1], (binder.NATIVE, "bind", interface.name))

    def test_third_party_takeover_is_never_displaced(self):
        interface = self.b.make_interface()
        self.b.takeover = True
        with self.assertRaisesRegex(binder.BindError, "ROLLBACK FAILED"):
            self.b.attach(interface)
        self.assertEqual(self.b.owner(interface), "other")
        self.assertEqual(self.b.events[-1], (binder.NATIVE, "bind", interface.name))

    def test_changed_identity_blocks_every_write(self):
        interface = self.b.make_interface()
        (interface.resolve().parent / "idProduct").write_text("ffff")
        with self.assertRaisesRegex(binder.BindError, "identity changed"):
            binder.Binder.write(self.b, interface, "cp210x", "unbind")

    def test_aggregate_failure_does_not_touch_other_devices(self):
        interface = self.b.make_interface()
        self.b.make_interface(name="1-3:1.0", vendor="ffff")
        self.b.busy = True
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(self.b.bind_all(), 1)
        self.assertEqual(self.b.owner(interface), "cp210x")
        self.assertEqual(len(self.b.events), 1)


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / "persistence")
        self.addCleanup(self.temp.cleanup)
        self.kdir = Path(self.temp.name) / "future-9.8.7/build"
        (self.kdir / "include/generated").mkdir(parents=True)
        self.metadata = self.kdir / "include/generated/compile.h"
        self.metadata.write_text('#define LINUX_COMPILER "x86_64-linux-gnu-gcc-17 (Future) 17.1.0"\n')

    def test_future_target_and_target_compiler_not_running_kernel(self):
        command = build.build_command(self.kdir, Path(self.temp.name),
                                      lambda name: f"/usr/bin/{name}",
                                      lambda *a, **k: types.SimpleNamespace(stdout="17.1.0\n"))
        self.assertEqual(command[:3], ["make", "-C", str(self.kdir)])
        self.assertIn("CC=/usr/bin/x86_64-linux-gnu-gcc-17", command)
        self.assertNotIn("gcc-13", " ".join(command))
        self.assertEqual(command[-2:], ["W=1", "modules"])

    def test_fallback_major_and_missing_or_wrong_compiler_fail_closed(self):
        command = build.build_command(self.kdir, "/fake/module",
                                      lambda name: "/usr/bin/gcc-17" if name == "gcc-17" else None,
                                      lambda *a, **k: types.SimpleNamespace(stdout="17\n"))
        self.assertIn("CC=/usr/bin/gcc-17", command)
        with self.assertRaisesRegex(ValueError, "require gcc-17"):
            build.build_command(self.kdir, "/fake/module", lambda name: None)
        with self.assertRaisesRegex(ValueError, "require gcc-17"):
            build.build_command(self.kdir, "/fake/module", lambda name: "/fake/gcc",
                                lambda *a, **k: types.SimpleNamespace(stdout="13\n"))

    def test_metadata_is_not_shell_code(self):
        self.metadata.write_text('#define LINUX_COMPILER "$(touch /root/no) gcc-13"\n')
        with self.assertRaisesRegex(ValueError, "unsupported target compiler"):
            build.build_command(self.kdir, "/fake/module")

    def test_clang_target_does_not_fall_back_to_gcc(self):
        self.metadata.write_text('#define LINUX_COMPILER "Ubuntu clang version 19.1.0"\n')
        command = build.build_command(self.kdir, "/fake/module",
                                      lambda name: "/usr/bin/clang-19" if name == "clang-19" else None,
                                      lambda *a, **k: types.SimpleNamespace(stdout="19.1.0\n"))
        self.assertIn("LLVM=1", command)
        self.assertIn("CC=/usr/bin/clang-19", command)

    def test_dkms_argument_and_target_flow(self):
        with patch.object(build, "build_command", return_value=["fake-make"]) as command, \
                patch.object(build.subprocess, "run", return_value=types.SimpleNamespace(returncode=0)):
            self.assertEqual(build.main(["--kernelver", "9.8.7-future", "--module-dir",
                                         "/fake/build", "KERNELRELEASE=9.8.7-future"]), 0)
        self.assertEqual(command.call_args.args[0], Path("/lib/modules/9.8.7-future/build"))

    def test_dkms_config_substitutes_future_kernel_instead_of_uname(self):
        result = subprocess.run(["/bin/bash", "-c",
                                 'dkms_tree=/fake/dkms; kernelver=9.8.7-future; source "$1"; '
                                 'printf "%s\\n" "${MAKE[0]}" "$AUTOINSTALL" "${BUILT_MODULE_NAME[0]}"',
                                 "test", str(ROOT / "native/dkms.conf")],
                                check=True, capture_output=True, text=True)
        self.assertIn("--kernelver 9.8.7-future", result.stdout)
        self.assertIn("/fake/dkms/ax860i-native/0.1.0/build", result.stdout)
        self.assertNotIn("uname", result.stdout)
        self.assertIn("\nyes\nax860i_native\n", result.stdout)

    def test_missing_target_metadata_and_failed_make_do_not_report_success(self):
        self.metadata.unlink()
        with patch.object(build.subprocess, "run") as run, \
                contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as failure:
                build.main(["--kdir", str(self.kdir), "--module-dir", self.temp.name])
        self.assertEqual(failure.exception.code, 1)
        run.assert_not_called()
        with patch.object(build, "build_command", return_value=["fake-make"]), \
                patch.object(build.subprocess, "run", return_value=types.SimpleNamespace(returncode=2)):
            self.assertEqual(build.main(["--kernelver", "9.8.7-future", "--module-dir",
                                         self.temp.name]), 2)


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / "persistence")
        self.addCleanup(self.temp.cleanup)
        self.stage = Path(self.temp.name) / "release"
        self.hash = package.stage_release(self.stage)

    def test_entire_curated_source_tree_has_no_private_paths_or_binary_artifacts(self):
        # Cover docs and tests too, not just installer payload or provenance.
        # Assemble the home marker so this fixture does not match itself.
        directories = (ROOT, ROOT / "native", ROOT / "native/tests",
                       ROOT / "persistence", ROOT / "ax860i")
        count = 0
        for directory in directories:
            for path in directory.iterdir():
                if not path.is_file():
                    continue
                count += 1
                with self.subTest(file=str(path.relative_to(ROOT))):
                    self.assertFalse(path.is_symlink())
                    self.assertNotIn(b"/" + b"home/", path.read_bytes())
                    self.assertNotIn(b"/" + b"tmp/opencode", path.read_bytes())
                    self.assertNotIn(path.suffix, (".ko", ".o", ".pyc", ".dll", ".zip",
                                                   ".jsonl", ".log"))
        self.assertGreaterEqual(count, 25)

    def test_manifest_source_only_and_root_owned_runtime_paths(self):
        payload = package.verified_payload(self.stage, self.hash)
        self.assertEqual(set(payload), set(package.file_map()))
        self.assertFalse(any(path.endswith((".ko", ".o", ".pyc", ".jsonl")) for path in payload))
        for path, data in payload.items():
            self.assertTrue(path.startswith(("/usr/src/", "/usr/local/", "/etc/systemd/system/",
                                             "/etc/udev/rules.d/")))
            self.assertNotIn(b"/" + b"home/", data)
            self.assertEqual((self.stage / path.lstrip("/")).stat().st_mode & 0o777,
                             package.mode_for(path))
        self.assertNotIn("transport.py", " ".join(payload))

    def test_tamper_and_unapproved_manifest_rejected(self):
        with self.assertRaisesRegex(ValueError, "manifest hash"):
            package.verified_payload(self.stage, "0" * 64)
        target = self.stage / "usr/local/libexec/ax860i-native-bind"
        target.write_text("tampered")
        with self.assertRaisesRegex(ValueError, "release changed"):
            package.verified_payload(self.stage, self.hash)

    def test_release_no_overwrite_and_host_conflict_preserved(self):
        with self.assertRaises(FileExistsError):
            package.stage_release(self.stage)
        root = Path(self.temp.name) / "fake-root"
        existing = root / "usr/local/bin/ax860i-sensors"
        existing.parent.mkdir(parents=True)
        existing.write_text("original")
        with self.assertRaisesRegex(ValueError, "will not overwrite"):
            package.require_absent(root, ["/usr/local/bin/ax860i-sensors"])
        self.assertEqual(existing.read_text(), "original")

    def test_exact_allowlist_rejects_extra_paths_and_symlink_sources(self):
        manifest_file = self.stage / "manifest.json"
        manifest = json.loads(manifest_file.read_bytes())
        manifest["files"]["/etc/shadow"] = {"sha256": "0", "mode": 0o644}
        manifest_file.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "allowlist"):
            package.verified_payload(self.stage, package.digest(manifest_file.read_bytes()))

    def test_leaf_symlink_is_not_an_install_source(self):
        path = self.stage / "usr/local/libexec/ax860i-native-bind"
        path.unlink()
        path.symlink_to(ROOT / "persistence/ax860i-native-bind")
        with self.assertRaisesRegex(ValueError, "symlink"):
            package.verified_payload(self.stage, self.hash)

    def test_destination_symlink_never_redirects_install(self):
        root = Path(self.temp.name) / "fake-root"
        (root / "usr/local").mkdir(parents=True)
        (root / "usr/local/bin").symlink_to(self.stage)
        with self.assertRaisesRegex(ValueError, "symlink destination"):
            package.require_absent(root, ["/usr/local/bin/ax860i-sensors"])

    def test_internal_directory_and_manifest_symlinks_are_rejected(self):
        directory = self.stage / "usr/local/libexec"
        directory.rename(self.stage / "saved-libexec")
        directory.symlink_to(self.stage / "saved-libexec", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            package.verified_payload(self.stage, self.hash)
        manifest = self.stage / "manifest.json"
        manifest.rename(self.stage / "saved-manifest.json")
        manifest.symlink_to(self.stage / "saved-manifest.json")
        with self.assertRaisesRegex(ValueError, "manifest.*symlink"):
            package.verified_payload(self.stage, self.hash)

    def test_copy_payload_creates_only_manifest_files_with_expected_modes(self):
        root = Path(self.temp.name) / "fake-root"
        root.mkdir()
        payload = package.verified_payload(self.stage, self.hash)
        package.require_absent(root, list(payload))
        package.copy_payload(payload, root)
        self.assertEqual({str(p.relative_to(root)) for p in root.rglob("*") if p.is_file()},
                         {p.lstrip("/") for p in payload})
        for target, data in payload.items():
            path = root / target.lstrip("/")
            self.assertEqual(path.read_bytes(), data)
            self.assertEqual(path.stat().st_mode & 0o777, package.mode_for(target))

    def test_new_directory_modes_ignore_restrictive_umask(self):
        root = Path(self.temp.name) / "restrictive-root"
        root.mkdir()
        old_umask = os.umask(0o077)
        try:
            package.copy_payload({"/usr/local/bin/ax860i-sensors": b"fixture"}, root)
        finally:
            os.umask(old_umask)
        for name in ("usr", "usr/local", "usr/local/bin", "usr/local/bin/ax860i-sensors"):
            self.assertEqual((root / name).stat().st_mode & 0o777, 0o755)

    def test_apply_action_allowlist_without_host_writes_or_activation(self):
        with patch.object(package.os, "geteuid", return_value=0), \
                patch.object(package, "require_absent") as preflight, \
                patch.object(package, "copy_payload") as copy, \
                patch.object(package.shutil, "which", return_value="/fake/program"), \
                patch.object(package.Path, "is_file", return_value=True), \
                patch.object(package.subprocess, "run") as run, \
                contextlib.redirect_stdout(io.StringIO()):
            package.apply_release(self.stage, self.hash, "9.8.7-future")
        preflight.assert_called_once()
        copy.assert_called_once()
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual([command[1] for command in commands],
                         ["add", "build", "install", "daemon-reload", "control", "enable"])
        self.assertEqual(commands[1][-2:], ["-k", "9.8.7-future"])
        for command in commands:
            self.assertFalse(set(command) & {"start", "trigger", "--now", "modprobe", "insmod", "rmmod"})

    def test_companion_imports_without_transport_or_project_dependency(self):
        lib = self.stage / "usr/local/lib/ax860i-sensors"
        result = subprocess.run(["/usr/bin/python3", "-I", "-c",
                                 "import sys; sys.path.insert(0, sys.argv[1]); "
                                 "from ax860i.native_readout import main; "
                                 "assert 'ax860i.transport' not in sys.modules; main(['--help'])",
                                 str(lib)], check=True, capture_output=True, text=True,
                                cwd=self.stage)
        self.assertIn("--json", result.stdout)

    def test_apply_requires_root_before_subprocess_or_copy(self):
        with patch.object(package.os, "geteuid", return_value=1000), \
                patch.object(package, "copy_payload") as copy, \
                patch.object(package.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "requires root"):
                package.apply_release(self.stage, self.hash, "9.8.7-future")
        copy.assert_not_called()
        run.assert_not_called()

    def test_approval_bootstrap_checks_installer_bytes_before_execution(self):
        bootstrap = (
            'import hashlib,sys; from pathlib import Path; '
            'p,h=sys.argv[1:3]; b=Path(p).read_bytes(); '
            'hashlib.sha256(b).hexdigest()==h or sys.exit("installer hash mismatch"); '
            'sys.argv=[p]+sys.argv[3:]; '
            'exec(compile(b,p,"exec"),{"__name__":"__main__","__file__":p})'
        )
        installer = ROOT / "persistence/package.py"
        command = ["/usr/bin/python3", "-I", "-B", "-c", bootstrap, str(installer)]
        valid = subprocess.run(command + [package.digest(installer.read_bytes()), "--help"],
                               capture_output=True, text=True, check=True, cwd=self.stage)
        self.assertIn("--manifest-sha256", valid.stdout)
        invalid = subprocess.run(command + ["0" * 64, "--help"],
                                 capture_output=True, text=True, cwd=self.stage)
        self.assertEqual(invalid.returncode, 1)
        self.assertEqual(invalid.stdout, "")
        self.assertIn("installer hash mismatch", invalid.stderr)

    def test_rules_and_services_no_udev_run_or_instance_path_injection(self):
        rules = (ROOT / "persistence/90-ax860i-native.rules").read_text()
        for expected in ('ACTION=="add"', 'ENV{DEVTYPE}=="usb_interface"',
                         'ATTR{bInterfaceNumber}=="00"', 'ATTRS{idVendor}=="1b1c"',
                         'ATTRS{idProduct}=="1c00"', 'TAG+="systemd"',
                         'ax860i-native-bind@.service'):
            self.assertIn(expected, rules)
        self.assertNotIn("RUN", rules)
        self.assertNotIn('ACTION=="bind"', rules)
        for name in (package.SERVICE, "ax860i-native-bind@.service"):
            unit = (ROOT / "persistence" / name).read_text()
            self.assertNotIn("RemainAfterExit=yes", unit)
            self.assertNotIn("%I", unit)
            self.assertNotIn("BindsTo=", unit)
            self.assertIn("RuntimeDirectoryPreserve=yes", unit)


if __name__ == "__main__":
    unittest.main()
