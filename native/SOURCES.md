# Public source provenance and implementation notes

## Linux UART profile

Register definitions and flow-control masks in `ax860i_usb_plan.h` were adapted
from Linux **v7.0**, `drivers/usb/serial/cp210x.c`:

https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/usb/serial/cp210x.c?h=v7.0

Recorded upstream git blob: `333b8b4637048375499d0fcb62512e3cd67c5143`.
This is a provenance identity from the development source record, not a newly
verified download or a SHA256 of this release. Upstream's SPDX identifier is
**GPL-2.0**. Retained notices:

* Copyright (C) 2005 Craig Shelley (craig@microtron.org.uk)
* Copyright (C) 2010-2021 Johan Hovold (johan@kernel.org)

The independent request plan restricts the profile to old CP2102/CP2103,
requires an exact one-byte part query, preserves flow limits/unrelated bits,
refuses inherited byte filtering, selects 115200 8N1 and active DTR/RTS, and
uses disable/enable rather than purge to reset event insertion. It is not a
complete USB-serial-core TTY-open implementation or a claim of all-chip support.
Failed-init cleanup uses the same bounded IFC_DISABLE request, not a new PSU
command or persistent setting. Transport, telemetry, ownership and tests are
independent implementations; this description is not a legal clearance claim.

## PSU protocol and estimate

Static behavior and formula observations from **Windows Corsair Link 4.9.9.3**
informed wire encoding, monitoring selectors, register mapping, five-page power
aggregation and the input-power/efficiency curve implemented in
`../ax860i/protocol.py`, `../ax860i/native_readout.py` and the native headers.
The codec is algorithmically implemented rather than a copied lookup table.
The curve has separate coefficients below/above 180 V and a ratio clamp.
It must not be described as calibrated measured input power or efficiency.

No Windows ZIP, DLL, decompiled CIL excerpt, reverse-engineering database,
private evidence path or live capture is distributed here. No endorsement or
rights determination is implied by this provenance citation. The maintainer has
authorized whole-release GPL-2.0-only licensing; this records that release decision,
not a formal legal certification of source rights or licensing coverage.

## Release preparation (2026-10-22)

These release copies add SPDX notices where missing in independent scripts,
tests, configuration and rules; this public provenance replaces local evidence
references. The packaging privacy test now covers every payload file, with no
provenance exemption. Driver behavior and installer behavior are unchanged.
The adapted UART header retains its original SPDX and author notices; the
Linux register/mask adaptation is identified prominently at its top.
