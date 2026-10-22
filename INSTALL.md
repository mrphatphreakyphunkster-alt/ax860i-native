# Installation and removal — manual review required

These are instructions, not actions run by tests or release preparation.
Assumptions: Ubuntu/Debian, Python 3.10+, DKMS, make, a compiler matching the
**target** headers, systemd/udev, fuser and modprobe in the script's fixed
`/usr/bin` and `/usr/sbin` locations. Obtain missing dependencies separately;
this project does not install packages. Verify the source archive checksum
against a separately trusted digest before extraction. An unsigned checksum
checks integrity, not publisher identity.

## 1. Unprivileged review and staging

In a freshly extracted project root, without sudo:

```sh
sha256sum -c SHA256SUMS
python3 -B persistence/package.py --help
python3 -B persistence/package.py
python3 -B -m unittest -v test_persistence test_native_readout
python3 -B -m unittest discover -s native/tests -v
ROOT="$PWD"
KERNEL="$(uname -r)"
MANIFEST_SHA256="$(python3 -B persistence/package.py --stage "$ROOT/stage-0.1.0")"
sha256sum persistence/package.py
```

The no-argument command is the **plan**, not an install or a host preflight.
It prints all intended destinations. Stage creates a **new directory whose
parent is inside this extracted project**. It refuses an existing directory;
do not stage over an earlier approval. `--without-companion` can be added to
the staging command if desired. Staging regenerates hashes after public
provenance sanitization; never reuse a development payload manifest.

Inspect every staged file and `stage-0.1.0/manifest.json`. Record its printed
SHA256, plus the **separate SHA256 of persistence/package.py**. The payload
manifest does not authenticate the installer. Retain both approval digests
outside the mutable project. Set `INSTALLER_SHA256` to that reviewed 64-hex
installer digest, and ensure `MANIFEST_SHA256` is the separately approved
manifest digest before proceeding. A digest calculated from unreviewed code
is not approval. `SHA256SUMS` covers source files; it is not the payload manifest.

`KERNEL` above selects the running kernel explicitly. To target another kernel,
set it to that one exact installed release and review its headers/compiler;
do not use an all-kernels operation. `native/build_module.py` reads the target
`compile.h`, requires matching GCC/Clang major, and builds with `W=1`.
Do not build in the pristine source tree used for approval (see TESTING.md).

## 2. Separate approval: install/build/enable, not live activation

Review destination absence, root ownership/permissions and symlinks; the
installer refuses any existing package destination. It is not an upgrade
procedure. Stop if this version or a source-equivalent prototype is installed.
Inspect actual DKMS hooks/framework overrides, autoinstall sentinels and module
trust/signing policy. Only Secure Boot disabled has been tested. Normal DKMS
signing may use/create signing artifacts; stop for separate approval if trust
or enrollment is needed. Do not read/copy private keys or bypass signatures.

Approve the exact manifest file list, DKMS state/build/index/signing effects,
unit/rule reload and **future** boot/hotplug enablement. Stop competing serial
readers yourself before any future binding; this package never kills them.

After approval only, the following isolated bootstrap checks the approved
installer bytes and executes **those same bytes**, not a second read. Variables
must still hold the reviewed absolute project path and approved digests.
Use `/usr/bin/python3`, not a project interpreter. First test the hash gate
without sudo using `--help`; then repeat with sudo and `--apply` only if approved:

```sh
BOOTSTRAP='import hashlib,sys; from pathlib import Path; p,h=sys.argv[1:3]; b=Path(p).read_bytes(); hashlib.sha256(b).hexdigest()==h or sys.exit("installer hash mismatch"); sys.argv=[p]+sys.argv[3:]; exec(compile(b,p,"exec"),{"__name__":"__main__","__file__":p})'
/usr/bin/python3 -I -B -c "$BOOTSTRAP" "$ROOT/persistence/package.py" "$INSTALLER_SHA256" --help
# PRIVILEGED: execute only after the exact scope/digests/kernel are approved.
sudo /usr/bin/python3 -I -B -c "$BOOTSTRAP" "$ROOT/persistence/package.py" "$INSTALLER_SHA256" --apply "$ROOT/stage-0.1.0" --manifest-sha256 "$MANIFEST_SHA256" --kernelver "$KERNEL"
```

Apply copies the source to `/usr/src/ax860i-native-0.1.0`, installs the binder
under `/usr/local/libexec`, the two units under `/etc/systemd/system`, and the
one rule under `/etc/udev/rules.d`. Optional companion files go under
`/usr/local/bin` and `/usr/local/lib/ax860i-sensors`. See the exact manifest.
It runs DKMS add, build and install for **only `$KERNEL`**, daemon-reload,
udev rule reload and enable of `ax860i-native-bind.service`. DKMS additionally
changes its version state and the selected kernel's module/index files.
AUTOINSTALL=yes permits normal future DKMS hooks; it guarantees no future ABI.

No service start, udev trigger, module load, explicit unload/unbind or reboot
is requested by apply. Recheck framework overrides: DKMS policy is host-owned.
Enabling persistence authorizes future arrivals/boots to bind hardware.
Apply is not transactional after copies: if DKMS fails, stop, retain the
manifest/error and inventory partial state; do not blindly rerun or overwrite.

## 3. Separate approval: live activation/testing

An explicit `sudo systemctl start ax860i-native-bind.service` is a **live USB
action**: it can load the module and unbind cp210x on exact matching interfaces.
Do not run it until the live plan is approved and competing readers are stopped.
The helper enumerates matching devices, refuses busy/missing TTYs, and will
not repair/reload an already-native interface. A running prototype requires a
separate exact-device rebind/module-replacement plan, not installer reruns.

After approved activation, inspect service/journal status and hwmon labels,
then run `ax860i-sensors --help`, `ax860i-sensors --once` or
`ax860i-sensors --json` without sudo. The companion reads sysfs only. A failure
to expose a fresh total/voltage/raw power snapshot is not success. Reboot,
replug, suspend/reset and loading a newly built binary are separate tests;
they are not validated by unit tests or an already-native binder no-op.

## Recovery/removal — targeted and separately approved

There is no automatic uninstaller. Preserve the approved manifest and inspect
`dkms status` to inventory **each exact kernel** this version was installed for.

1. After approval, disable the boot service, prevent new hotplug instances,
   and let any running oneshot finish its rollback. Remove only this package's
   verified rule and reload rules (not events); do not interrupt a live recovery.
   Prevent arrivals while removing; a rule reload alone does not stop an
   already queued instance. Do not trigger the USB bus.
2. Configuration removal is **not active USB restoration**. If restoring
   cp210x is desired, obtain separate live-action approval, validate the exact
   current device identity/owner and perform narrow native unbind / original
   driver bind. An originally unbound interface should return to unbound.
   Do not unload global cp210x or infer a device path from this document.
   Reopening the restored TTY may be needed to configure its UART.
3. After approving one inventoried kernel, use
   `sudo dkms remove -m ax860i-native -v 0.1.0 -k "$KERNEL"` with that exact
   approved value. Repeat only for separately inventoried/approved kernels;
   never use `--all`. Review collisions/original-module restoration. A loaded
   native module may remain in memory until separately approved unload/reboot.
4. Check installed bytes against the retained manifest before removing each
   listed file; changed files need review. Remove this service's exact enable
   symlink and reload systemd. Remove only empty package-specific directories,
   never recursively delete shared trees or unreviewed DKMS state. Allow the
   runtime lock directory to expire at reboot; never unlink a live lock.
5. Do not change other rules, collectors/Netdata, system packages or signing
   keys. No private-key deletion or unrelated service restoration is needed.

The binder attempts original-owner recovery on probe/snapshot failure only
while identity/ownership remain safe. A recovery failure is reported, not
hidden or retried. Review it manually instead of broad USB resets.
