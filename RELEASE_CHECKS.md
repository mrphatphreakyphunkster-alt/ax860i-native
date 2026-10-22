# Source verification — 2026-10-22

The following checks were recorded during local draft preparation, without
installation or hardware activation. These historical checks are distinct from
the final publication and downloaded-asset verification described below:

| Check | Result |
| --- | --- |
| Clean curated root: `python3 -B -m unittest -v test_persistence test_native_readout` | 50 passed (39 persistence, 11 readout) |
| Clean curated root: `python3 -B -m unittest discover -s native/tests -v` | 6 passed |
| `systemd-analyze verify` on both supplied service units | Exit 0; an unrelated host-unit KillMode warning was emitted; no unit changed |
| `udevadm verify` on supplied rule | 1 success, 0 failures |
| Installer `--help` and no-argument plan | Exit 0; no installation performed |
| Explicit current-header build, separate build-check source copy | Built with `W=1` for 7.0.0-38-generic; no load/install |
| Build caveats | pahole version differs; BTF skipped because vmlinux unavailable |
| Fresh project-local staging in the separate copy | Completed; sanitized provenance included; no privileged apply |
| Installer hash-gate regression | Matching digest + `--help` accepted; wrong digest refused before execution |
| Privacy coverage | Regression covers docs/tests and all curated directories; payload SOURCES has no exemption |

56 unittest cases are offline checks, **not** 56 hardware scenarios. Native
harnesses include 196608 LINEAR11 vectors and 100 abstract lifecycle races;
these do not replace live lockdep/KASAN or PM/reset testing.

## Artifact acceptance procedure

The maintainer's local preparation tool checks a positive file allowlist and
private-path/host-identifier/credential-pattern exclusions, writes deterministic
relative `SHA256SUMS`, and archives only source/docs/tests. Tar owners/names and
timestamps are normalized (0/empty), with no machine pathname metadata.
It validates all archive entries and compares a separate re-extraction byte
for byte. The standalone `.tar.gz.sha256` authenticates archive bytes only,
not a maintainer identity; no signing key is created or used.

After archive creation, acceptance requires rerunning both test commands and
`sha256sum -c SHA256SUMS` inside the separate extraction, with `PYTHONPATH`
unset. Tests resolve ROOT relative to their own files and depend on no
historical captures or original development checkout. The final delivery
record supplies that extraction result and the archive hash/size/file count.
No private build logs or staged installation duplicates belong in the archive.

The maintainer has authorized publication with whole-release GPL-2.0-only
licensing and GitHub Issues contact. Technical checks are not legal certification.
Cold boot/replug, PM/reset, future kernels, Secure Boot enabled, and a newly
installed binary's live load remain unverified.

For this public prerelease, rerun the 50 persistence/readout tests and 6 native
tests unprivileged on the final clean tree and on the independently extracted
source asset. Verify all 35 allowlisted files against the final commit, and all
34 payload digests against SHA256SUMS. After publication, download both release
assets, check the archive digest against the sidecar and local asset, compare
every extracted file with the final commit, and rerun all 56 offline tests.
Publication command results and hashes are kept outside the source repository.
