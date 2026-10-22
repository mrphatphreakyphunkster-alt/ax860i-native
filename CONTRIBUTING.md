# Contributing

Repository: https://github.com/mrphatphreakyphunkster-alt/ax860i-native
Maintainer: **mrphatphreakyphunkster-alt**. Use
[GitHub Issues](https://github.com/mrphatphreakyphunkster-alt/ax860i-native/issues)
for public contact and bug reports; support is best-effort.
This whole release is GPL-2.0-only; contributions must be compatible with that
license. Retain all existing attribution and do not invent a Signed-off-by identity.

Please submit source-only changes with focused offline tests and an explanation
of protocol/ownership assumptions. Run TESTING.md without root. Do not bundle
vendor binaries, decompiler output, private captures, device serials, hostnames,
keys or installation reports. Synthetic fixtures should be anonymous.

Keep exact hardware allowlists and fail-closed behavior. Do not widen USB IDs,
add controls or claim new hardware support based on a mock. Hardware tests
need an explicit plan, user approval, recovery path and separately reported
kernel/device coverage. Separate offline checks from live activation.

Behavioral changes after this release require a reviewed version bump across
DKMS and installer paths, not replacement of an installed fixed 0.1.0 tree.
