#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build against the explicit target headers, never the running kernel by accident."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess


def build_command(kdir, module_dir, which=shutil.which, run=subprocess.run):
    kdir, module_dir = Path(kdir), Path(module_dir)
    metadata = (kdir / "include/generated/compile.h").read_text(encoding="ascii")
    compiler = re.search(r'^#define\s+LINUX_COMPILER\s+"([^"\n]+)"', metadata, re.M)
    if not compiler:
        raise ValueError("target headers do not identify LINUX_COMPILER")
    # Strict allowlist: metadata is data, not shell code. Do not guess gcc-13.
    gcc = re.match(r"([A-Za-z0-9_+.-]*gcc)-(\d+)(?:\s|$)", compiler[1])
    clang = re.search(r"\bclang version (\d+)\.", compiler[1])
    if gcc:
        major = gcc[2]
        candidates = [gcc[0].strip(), f"gcc-{major}"]
        family = "gcc"
    elif clang:
        major = clang[1]
        candidates = [f"clang-{major}"]
        family = "clang"
    else:
        raise ValueError(f"unsupported target compiler: {compiler[1]}")
    for candidate in dict.fromkeys(candidates):
        executable = which(candidate)
        if not executable:
            continue
        result = run([executable, "-dumpversion"], check=True, capture_output=True,
                     text=True, timeout=10)
        if result.stdout.strip().split(".")[0] != major:
            continue
        command = ["make", "-C", str(kdir), f"M={module_dir}",
                   f"CC={executable}", "W=1"]
        if family == "clang":
            command.append("LLVM=1")
        return command + ["modules"]
    raise ValueError(f"target headers require {family}-{major}; install it only with approval")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernelver")
    parser.add_argument("--kdir", type=Path)
    parser.add_argument("--module-dir", type=Path, required=True)
    # DKMS 3.0.11 appends KERNELRELEASE=<target> to MAKE commands.
    parser.add_argument("dkms_arguments", nargs="*")
    args = parser.parse_args(argv)
    if bool(args.kernelver) == bool(args.kdir):
        parser.error("supply exactly one of --kernelver or --kdir")
    if args.kernelver and not re.fullmatch(r"[A-Za-z0-9_+.-]+", args.kernelver):
        parser.error("invalid target kernel version")
    if any(value != f"KERNELRELEASE={args.kernelver}" for value in args.dkms_arguments):
        parser.error("unexpected DKMS argument")
    kdir = args.kdir or Path("/lib/modules") / args.kernelver / "build"
    try:
        command = build_command(kdir, args.module_dir)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(1, f"ax860i build refused: {exc}\n")
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
