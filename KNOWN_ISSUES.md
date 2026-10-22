# Limitations and hardware-test boundaries

* Experimental prerelease, not upstream-ready. One AX860i/dongle combination
  tested; no modern HXi/RMi or arbitrary cp210x compatibility claim.
* Native sweeps cover five pages sequentially. Cache publication is complete
  or fail-closed, but physical readings are not simultaneous. Companion
  before/after power comparisons cannot guarantee atomicity or eliminate ABA.
* Raw AC current is hidden due to inconsistency with raw AC power. Raw power
  remains labeled raw; calibration is unknown. Estimated input power/efficiency
  is an optional software curve, not a hardware efficiency sensor.
* Auxiliary V/I and per-connector mapping are unfinished. No fan/OCP control.
* Protocol errors poison the device; stale/failed reads return errors rather
  than serving indefinitely cached values. PM/reset fails closed until an
  explicitly approved manual rebind. The binder intentionally does nothing to
  already-native devices, even unhealthy ones; restarting it is not a repair.
* Autosuspend continuity is not validated; the driver does not advertise
  autosuspend support. Do not infer suspend/reset safety from offline tests.
* Current-kernel persistence was configured on the development host, not
  cold-boot/replug validated. Future-kernel compatibility is untested. DKMS
  autoinstall depends on installed hooks, matching headers/compiler and trust.
* The newly installed DKMS binary is not yet live-tested. A source-equivalent
  prototype was used on hardware. Offline builds do not validate a new load.
* Secure Boot was tested **disabled only**. DKMS may use/generate normal signing
  artifacts according to its framework policy. No key enrollment, key reads,
  trust bypass or private-key deletion is part of this release procedure.
* The fuser idle check has an unavoidable check/open race. Users must stop
  competing serial readers themselves before binding; the helper refuses busy,
  missing or inaccessible TTYs and never kills processes. Avoid competing
  readers at future boots/arrivals too.
* Binder recovery restores original ownership only if identity and ownership
  checks still permit it. Third-party takeover is not displaced; recovery can
  fail or kernel teardown can block. Offline race models are not lockdep/KASAN.
* Apply is no-overwrite, not an upgrade tool or transactional DKMS rollback.
  A partial failure can leave source/runtime files installed. Keep the staged
  manifest and errors, stop and review rather than rerunning blindly.
* Paths and utilities assume Ubuntu/Debian (Python 3.10+, systemd/udev, DKMS).
  Other distributions, architectures, kernels and signing policies are not
  promised compatible. Version 0.1.0 is fixed in source and runtime paths.
