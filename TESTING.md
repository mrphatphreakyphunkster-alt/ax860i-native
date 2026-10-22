# Offline checks and evidence boundaries

From a clean extracted project root, **without root**:

```sh
sha256sum -c SHA256SUMS
python3 -B -m unittest -v test_persistence test_native_readout
python3 -B -m unittest discover -s native/tests -v
python3 -B persistence/package.py --help
python3 -B persistence/package.py
systemd-analyze verify persistence/ax860i-native-bind.service persistence/ax860i-native-bind@.service
udevadm verify persistence/90-ax860i-native.rules
```

Python tests use only stdlib and synthetic temporary fixtures. Native tests
need a C compiler and undefined-behavior sanitizer runtime. The six native
groups exercise exhaustive LINEAR11 conversion, framing/cache/transaction
logic, UART allowlists/unwind, five-page snapshots, compiled production hwmon
mapping/visibility, abstract lifecycle races and static teardown-order guards.
The latter two share a group count of six overall, not live race validation.

Persistence tests mock host writes and subprocesses in the apply path. Tests
never install, reboot, open the USB/TTY, run an actual binder or alter services.
The root tests number 50 (39 persistence and 11 companion); combined with six
native groups this is 56 unittest cases. Historical full-project tests that
depend on private live captures are deliberately not shipped.

The `udevadm verify` subcommand is available on the tested systemd/udev 255;
if unavailable, report that check unverified. Do **not** substitute `test`,
`test-builtin`, `trigger` or a rule reload. Unit verification is offline, not
activation, and may warn about host dependencies that need separate review.

## Optional local build, separate from pristine source

Create a new sibling directory for build outputs, copy only `native/` there
(ordinary `cp -R` into a newly reviewed directory is sufficient), then from
the extracted root run, substituting that absolute new build directory:

```sh
KERNEL="$(uname -r)"
# BUILD_NATIVE must be the absolute native directory in your separate copy.
python3 -B native/build_module.py --kernelver "$KERNEL" --module-dir "$BUILD_NATIVE"
```

The helper checks compiler identity from the target headers and invokes kbuild
with `W=1`. No sudo, DKMS install or module load is needed. Keep `.ko`, object,
command and generated files out of the source release. Compilation does not
prove safe loading, PM/reset behavior or calibration. Future kernels require
new build and hardware review rather than a compatibility assumption.

For the local release preparation, the results (including actual counts,
current-header build and archive re-extraction checks) are recorded in
`RELEASE_CHECKS.md`. Checksums are regenerated only after documentation is
final; staging/approval uses a fresh manifest generated from these sanitized
sources, not a historical manifest. No staging duplicates enter the archive.
