# AX860i native sensors — 0.1.0 experimental prerelease

A Linux USB/hwmon driver for reading an **AX860i only**, with an optional
standard-library Python readout. This is a maintained experimental, source-only
release: not production-ready, accepted upstream, or endorsed/supported by
Corsair or Silicon Labs. Nothing installs or binds
hardware merely by extracting it or running the offline tests.

**Read [KNOWN_ISSUES.md](KNOWN_ISSUES.md) first.** Boot/replug, PM/reset,
future kernels and live loading of the installed DKMS artifact remain untested.
Installation is manual and nontransactional: a failed apply can leave partial
changes. Follow the hash/manifest gates and recovery instructions in
[INSTALL.md](INSTALL.md); passing offline tests does not establish hardware safety.

Repository: https://github.com/mrphatphreakyphunkster-alt/ax860i-native

Maintainer: **mrphatphreakyphunkster-alt**. Public contact and bug reports:
[GitHub Issues](https://github.com/mrphatphreakyphunkster-alt/ax860i-native/issues).

## Supported hardware and readings

The exact Corsair Link USB dongle **1b1c:1c00**, interface 0, with the checked
CP2102/CP2103 UART profile is required. The driver checks endpoints, part and
PSU model; it does not claim support for modern HXi/RMi devices or other PSUs.
Only **one physical AX860i/dongle combination** has been tested, on Ubuntu
24.04 with kernel **7.0.0-38-generic**, with Secure Boot disabled.

There are **14 read-only native inputs** (plus labels):

| Inputs | hwmon units |
| --- | --- |
| AC voltage and +12 V / +5 V / +3.3 V voltage | mV |
| +12 V / +5 V / +3.3 V current | mA |
| Raw AC input power, three rail powers, total DC output power | µW |
| Secondary ambient temperature | milli-degrees Celsius |
| PSU fan speed | RPM |

Total DC output sums power from **all five PSU pages**, including the two
auxiliary pages; it is not just the three displayed rails. AC input current
is intentionally hidden, including its label. It is not an additional public
mA input. The raw input-current/power inconsistency and calibration remain
unresolved. Auxiliary voltage/current and connector mapping are unfinished.

The optional companion reports raw input power separately from a
**curve-estimated input power and efficiency**. The estimate follows observed
Corsair Link 4.9.9.3 formula behavior, not a calibrated efficiency measurement.
Neither the driver sweep nor the companion's bounded coherence retry provides
an atomic physical sample. There is **no PSU fan or OCP control**; USB UART
setup and monitoring selectors still involve protocol writes.

## Start here

1. Read [KNOWN_ISSUES.md](KNOWN_ISSUES.md) before using hardware.
2. Verify `SHA256SUMS`, then run the offline checks in [TESTING.md](TESTING.md).
3. Review [INSTALL.md](INSTALL.md): first an unprivileged plan and fresh staged
   manifest, then a separately approved privileged installation, and finally a
   separately approved live activation. Installation refuses existing paths.

Current-kernel boot/replug persistence has been **configured** during development,
but actual cold boot, replug and future kernels are untested. The newly installed
DKMS binary has **not** been live-tested; the source-equivalent prototype was.
Preparing and publishing this source release does not change the working driver,
installed files or services.

## Licensing and provenance

**This whole release is licensed GPL-2.0-only**, consistent with
the native GPL component. Full GPLv2 text is in [COPYING](COPYING).
New independent Python, unit and rule files carry GPL-2.0-only SPDX notices in these
copies. Existing notices and upstream Craig Shelley/Johan Hovold credits are
retained. The upstream cp210x source's legacy identifier is `GPL-2.0`; it is
not silently relicensed here. See [native/SOURCES.md](native/SOURCES.md).

This is an independently implemented experimental driver, with adapted Linux
register definitions/masks as identified in the source. Windows binaries,
decompiled CIL, static-analysis artifacts, captures and private development
references are **not included**. Corsair and Silicon Labs names identify
compatibility/provenance only; no endorsement is implied. No legal clearance,
source-rights ownership or independent verification of all licensing coverage
is claimed. The maintainer has authorized GPL-2.0-only coverage for this release;
that authorization is not a formal legal certification or an endorsement by
upstream authors or vendors. Existing attribution is preserved; no personal
name or copyright ownership is invented.

Provided without warranty; see GPLv2 sections 11–12. Support is best-effort via
GitHub Issues, with no response-time or compatibility guarantee.
