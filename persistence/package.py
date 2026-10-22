#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Stage an immutable reviewed release; host installation needs --apply and root.

There is no automatic sudo, module load, service start, or package installation.
The apply mode installs/builds/enables persistence but deliberately does not bind
the currently active interface or replace its in-memory module.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parent.parent
SOURCE = "/usr/src/ax860i-native-0.1.0"
SERVICE = "ax860i-native-bind.service"
NATIVE_FILES = ("ax860i_native.c", "ax860i_core.h", "ax860i_sensors.h",
                "ax860i_snapshot.h", "ax860i_usb_plan.h", "Makefile",
                "build_module.py", "dkms.conf", "SOURCES.md")
RUNTIME_FILES = {
    "ax860i-native-bind": "/usr/local/libexec/ax860i-native-bind",
    SERVICE: f"/etc/systemd/system/{SERVICE}",
    "ax860i-native-bind@.service": "/etc/systemd/system/ax860i-native-bind@.service",
    "90-ax860i-native.rules": "/etc/udev/rules.d/90-ax860i-native.rules",
}


def file_map(companion=True):
    files = {f"{SOURCE}/{name}": ROOT / "native" / name for name in NATIVE_FILES}
    files.update({dest: ROOT / "persistence" / src for src, dest in RUNTIME_FILES.items()})
    if companion:
        files["/usr/local/bin/ax860i-sensors"] = ROOT / "persistence/ax860i-sensors"
        files.update({f"/usr/local/lib/ax860i-sensors/ax860i/{name}": ROOT / "ax860i" / name
                      for name in ("__init__.py", "protocol.py", "native_readout.py")})
    return files


def mode_for(path):
    return 0o755 if path in ("/usr/local/bin/ax860i-sensors",
                             "/usr/local/libexec/ax860i-native-bind") else 0o644


def digest(data):
    return hashlib.sha256(data).hexdigest()


def stage_release(destination, companion=True):
    destination = Path(destination).absolute()
    if not destination.parent.resolve().is_relative_to(ROOT):
        raise ValueError("stage parent must be inside this project")
    destination.mkdir(mode=0o755)  # Refuse to overwrite a previous release.
    manifest = {"package": "ax860i-native", "version": "0.1.0",
                "companion": companion, "files": {}}
    for target, source in sorted(file_map(companion).items()):
        data = source.read_bytes()
        path = destination / target.lstrip("/")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode_for(target))
        manifest["files"][target] = {"sha256": digest(data), "mode": mode_for(target)}
    data = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    (destination / "manifest.json").write_bytes(data)
    return digest(data)


def verified_payload(stage, expected_digest):
    stage = Path(stage)
    if stage.is_symlink():
        raise ValueError("stage must not be a symlink")
    if (stage / "manifest.json").is_symlink():
        raise ValueError("manifest must not be a symlink")
    raw = (stage / "manifest.json").read_bytes()
    if digest(raw) != expected_digest:
        raise ValueError("manifest hash differs from approved release")
    manifest = json.loads(raw)
    if (manifest.get("package"), manifest.get("version")) != ("ax860i-native", "0.1.0"):
        raise ValueError("unexpected release")
    if not isinstance(manifest.get("companion"), bool):
        raise ValueError("invalid companion selection")
    allowed = file_map(manifest["companion"])
    if manifest["files"].keys() != allowed.keys():
        raise ValueError("manifest not the exact installation allowlist")
    payload = {}
    for target, record in manifest["files"].items():
        path = stage / target.lstrip("/")
        if (any(part.is_symlink() for part in (path, *path.parents) if part != stage
                and part.is_relative_to(stage))
                or not path.resolve().is_relative_to(stage.resolve())):
            raise ValueError("source symlink in stage")
        data = path.read_bytes()
        if digest(data) != record["sha256"] or record["mode"] != mode_for(target):
            raise ValueError(f"release changed: {target}")
        payload[target] = data
    return payload


def require_absent(root, targets, trusted_uid=None):
    """All-or-nothing preflight; existing files are their own untouched backup."""
    for target in targets:
        path = Path(root) / target.lstrip("/")
        if os.path.lexists(path):
            raise ValueError(f"will not overwrite existing path: {path}")
        for parent in path.parents:
            if parent == Path(root):
                break
            if parent.is_symlink():
                raise ValueError(f"symlink destination parent: {parent}")
            if parent.exists() and (not parent.is_dir() or parent.stat().st_mode & 0o022):
                raise ValueError(f"unsafe destination parent: {parent}")
            if parent.exists() and trusted_uid is not None and parent.stat().st_uid != trusted_uid:
                raise ValueError(f"destination parent not root owned: {parent}")


def copy_payload(payload, root=Path("/")):
    created = []
    try:
        for target, data in sorted(payload.items()):
            path = root / target.lstrip("/")
            missing = []
            parent = path.parent
            while not parent.exists():
                missing.append(parent)
                parent = parent.parent
            for directory in reversed(missing):
                directory.mkdir(mode=0o755)
                directory.chmod(0o755)  # Do not inherit an approving user's restrictive umask.
                if os.geteuid() == 0:
                    os.chown(directory, 0, 0)
            with path.open("xb") as stream:
                created.append(path)
                stream.write(data)
                os.fchmod(stream.fileno(), mode_for(target))
                if os.geteuid() == 0:
                    os.fchown(stream.fileno(), 0, 0)
    except Exception:
        for path in reversed(created):
            path.unlink()
        raise


def apply_release(stage, expected_digest, kernelver):
    if os.geteuid() != 0:
        raise ValueError("--apply requires root AFTER explicit approval; no sudo is invoked")
    if not re.fullmatch(r"[A-Za-z0-9_+.-]+", kernelver):
        raise ValueError("invalid explicit target kernel version")
    payload = verified_payload(stage, expected_digest)
    require_absent(Path("/"), list(payload) + [SOURCE, "/usr/local/lib/ax860i-sensors",
                   f"/etc/systemd/system/multi-user.target.wants/{SERVICE}"], trusted_uid=0)
    # Fail before any copies if a required executable or target headers is missing.
    env = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LANG": "C", "HOME": "/root"}
    for program in ("dkms", "make", "systemctl", "udevadm", "fuser", "modprobe"):
        if not shutil.which(program, path=env["PATH"]):
            raise ValueError(f"missing {program}; package changes require separate approval")
    headers = Path("/lib/modules") / kernelver / "build/include/generated/compile.h"
    if not headers.is_file():
        raise ValueError(f"missing target headers: {headers}")
    # Avoid calling any code from the mutable project once root artifacts are copied.
    copy_payload(payload)
    commands = [
        ["/usr/sbin/dkms", "add", "-m", "ax860i-native", "-v", "0.1.0"],
        ["/usr/sbin/dkms", "build", "-m", "ax860i-native", "-v", "0.1.0", "-k", kernelver],
        ["/usr/sbin/dkms", "install", "-m", "ax860i-native", "-v", "0.1.0", "-k", kernelver],
        ["/usr/bin/systemctl", "daemon-reload"],
        ["/usr/bin/udevadm", "control", "--reload-rules"],
        ["/usr/bin/systemctl", "enable", SERVICE],
    ]
    for command in commands:
        # No --now, start, trigger, insmod, rmmod, or modprobe here.
        subprocess.run(command, check=True, env=env, cwd="/")
    print("Installed/enabled for future boot/arrival. Current module untouched.")
    print("Explicit service start is a SEPARATE live action, requiring approval.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, help="create a new project-local release tree")
    parser.add_argument("--without-companion", action="store_true")
    parser.add_argument("--apply", type=Path, help="install an approved staged release (root only)")
    parser.add_argument("--manifest-sha256")
    parser.add_argument("--kernelver")
    args = parser.parse_args(argv)
    try:
        if args.apply:
            if args.stage or args.without_companion or not args.manifest_sha256 or not args.kernelver:
                parser.error("--apply needs --manifest-sha256 and --kernelver only")
            apply_release(args.apply, args.manifest_sha256, args.kernelver)
        elif args.stage:
            print(stage_release(args.stage, not args.without_companion))
        else:
            print(json.dumps({"writes_pending_approval": sorted(file_map()),
                              "no_installation_performed": True}, indent=2))
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(1, f"persistence preparation/install failed: {exc}\n"
                    "If apply had begun, retain staged manifest; see README recovery.\n")


if __name__ == "__main__":
    main()
