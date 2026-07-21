# nyan Real VDD

A virtual display driver (Windows IddCx indirect display driver) built for
[nyan Real / Spatial Wall](https://github.com/8796n) — a 3DoF spatial wall for
XR glasses — and usable standalone by anything that needs scriptable virtual
monitors.

日本語版は [README.ja.md](README.ja.md) へ。

## Why another VDD?

Spatial Wall drives its walls by capturing virtual displays. Generic VDDs work,
but leave correctness gaps that show up as ghost monitors, index collisions and
monitors vanishing for seconds when display topology stalls. This driver fixes
those by design:

- **Cookie correlation.** Every monitor is plugged with a client-chosen 32-bit
  cookie which is burned into the EDID serial number (vendor `NYN`, product
  `0x3D0F`, serial = cookie). A client can always map an OS display back to its
  own request — connector indices are never used as identity.
- **No mandatory keepalive.** Monitors persist until explicitly unplugged.
  A stalled process or a busy topology change cannot cause spurious removals.
  Clients reconcile at startup (list, then unplug unknown cookies). An
  *optional* watchdog (>= 10 s, refreshed by any control call) is available for
  clients that want orphan cleanup.
- **Windows 11 24H2+ (IddCx 1.10 floor).** Everything older is end-of-life,
  so it is not carried as untested baggage. Precise present regions and
  realtime GPU priority for the frame path are always on; features beyond
  1.10 (e.g. IddCx 1.11 D3D12) are detected at runtime.

## Layout

| Path | What |
|---|---|
| `driver/` | The UMDF/IddCx driver (`nyanvdd.dll` + INF) |
| `include/nyanvdd_protocol.h` | The public control protocol (IOCTLs) — copy into clients |
| `cli/` | `nyanvddctl` — reference client / device-node installer |
| `scripts/` | build / sign / install / uninstall (PowerShell) |
| `docs/` | design and signing notes (Japanese) |

## Build

Requirements: Visual Studio (C++), Windows SDK, and the
[WDK](https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk)
with its Visual Studio extension.

```powershell
scripts\build.ps1              # -> out\package, out\nyanvddctl.exe
scripts\sign-dev.ps1           # self-signed dev/distribution signing
```

## Install & try

```powershell
# elevated PowerShell
scripts\install.ps1            # trust cert + pnputil + create device node

out\nyanvddctl.exe status
out\nyanvddctl.exe plug 1920x1080@120
out\nyanvddctl.exe list
out\nyanvddctl.exe unplug all

scripts\uninstall.ps1          # remove everything
```

The driver is user-mode only (no kernel code). With the self-signed flow the
installer adds the publisher certificate to the machine's Root and
TrustedPublisher stores — that is exactly why this repository is public with
full history: audit what you trust. Store-clean distribution via EV +
Microsoft attestation signing is a drop-in replacement for the signing step
(see `docs/signing.ja.md`).

## Control protocol

See [`include/nyanvdd_protocol.h`](include/nyanvdd_protocol.h) — a small
self-contained C header. Open the device interface
`{C9AC49E6-0024-4979-96C7-A3E4B911CFFC}` and issue `DeviceIoControl`:
`GET_STATUS`, `PLUG`, `UNPLUG`, `LIST`, `SET_WATCHDOG`. `nyanvddctl.cpp` is the
reference implementation.

## License

MIT. Portions derived from the Microsoft
[Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples)
IndirectDisplay sample (MIT). See [LICENSE](LICENSE).
