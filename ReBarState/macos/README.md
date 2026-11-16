# NvStrapsReBar for macOS (v0.3)

A native Intel macOS Terminal utility for the **upstream v0.3**
`NvStrapsReBar.ffs` driver. It reads and writes the same EFI configuration as
`NvStrapsReBar.exe`, so you can configure the driver without booting Windows.
The existing motherboard firmware modification is still required, and saved
changes take effect after rebooting. This utility does not flash firmware or
install a GPU driver.

Releases: https://github.com/HighDelay/NvStrapsReBar-MacOS/releases

The release ZIP includes the **original, unmodified upstream v0.3
`NvStrapsReBar.ffs`**, the macOS utility, a launcher, documentation, the MIT
license, and checksums. `UPSTREAM-DRIVER.txt` records the driver's source URL
and pinned SHA-256. Use the original upstream firmware installation procedure
if the driver is not already installed.

Requires Intel macOS 10.15 or later, an NVIDIA GPU visible to IOKit, and working
access to the driver's EFI variables. It uses IOKit and CoreFoundation; Wine,
Python, Homebrew, and kernel extensions are not needed. Apple Silicon is not
supported. The source and binary use the repository's MIT license.

## Run

Extract `NvStrapsReBar-macOS-v0.3.zip` and double-click
`NvStrapsReBar.command`. The launcher opens Terminal and uses `sudo`, which
asks for your macOS password so the menu can save settings. You can also run
the binary directly from Terminal:

```sh
./NvStrapsReBar --show
sudo ./NvStrapsReBar --interactive
```

In the menu, `E` toggles automatic sizes for Turing GPUs, `P` chooses the PCI
BAR selector, and `S` saves. `G` sets per-GPU sizes by PCI ID, subsystem ID,
or location. `O` sets a per-GPU BAR size mask override; `C` clears per-GPU
selectors; `H` toggles skipping S3 resume;
`M` toggles the global BAR size mask override. `V` previews the resulting
configuration, `R` reloads the saved configuration, and `Q` quits. Menu edits
stay in memory until `S`. `D`, followed by `S`, disables all changes and removes
the configuration variable, including per-GPU overrides and PCI resizing.

Command-line changes are also previews unless you include `--save`:

```sh
# Enable automatic GPU sizes, leaving PCI allocation to the motherboard.
./NvStrapsReBar --enable
sudo ./NvStrapsReBar --enable --save

# For boards that need the DXE driver to configure the PCI side too.
./NvStrapsReBar --enable --pci-size 64
sudo ./NvStrapsReBar --enable --pci-size 64 --save

# GPU 1: request 4 GiB, matched by its PCI location and subsystem.
./NvStrapsReBar --gpu-size 1 6 --scope location

# Disable the driver configuration before hardware or UEFI setup changes.
sudo ./NvStrapsReBar --disable --save
```

`--enable` preserves existing per-GPU selections and the PCI selector, matching
the Windows tool's `E` option. `--disable` clears the entire configuration.
`auto` on a per-GPU selector removes its explicit size; global automatic sizing
still applies if enabled. `exclude` excludes the matching GPU. `clear` removes
the selector, including its per-GPU mask override. `--help` lists all flags.

GPU size selectors and PCI size selectors use different scales:

| GPU selector | Size | GPU selector | Size |
| --- | --- | --- | --- |
| 0 | 64 MiB | 6 | 4 GiB |
| 1 | 128 MiB | 7 | 8 GiB |
| 2 | 256 MiB | 8 | 16 GiB |
| 3 | 512 MiB | 9 | 32 GiB |
| 4 | 1 GiB | 10 | 64 GiB |
| 5 | 2 GiB | auto / exclude / clear | See above |

| PCI selector | Behavior |
| --- | --- |
| 0 | System default; keep the motherboard's PCI allocation |
| 1–31 | Maximum BAR size of 2^N MiB (11 = 2 GiB, 12 = 4 GiB, 13 = 8 GiB) |
| 32 | Any BAR size supported by PCI devices |
| 64 | Configure PCI BARs for selected GPUs only |
| 65 | Configure GPU straps only; skip PCI BAR configuration |

The displayed largest current memory BAR is the allocation reported by macOS.
The requested GPU BAR is the saved/staged configuration for the next boot.
The DXE status is the firmware driver's last reported checkpoint, not a
per-application performance test. NVIDIA's application profiles and the
operating system's GPU driver still determine how applications use ReBAR.

## Backups and restoration

Every write first creates an exact binary backup in `NvStrapsReBar-backups`
under the current directory. `--backup-dir PATH` selects another directory.
A zero-byte backup means the variable did not exist. Saves check for concurrent
changes and verify the firmware readback. They use only
`e3ee4a27-e2a2-4435-bba3-184ccad935a8:NvStrapsReBar`; they do not modify boot
arguments or other configuration variables.

```sh
./NvStrapsReBar --export current.bin
./NvStrapsReBar --inspect current.bin
./NvStrapsReBar --restore current.bin
sudo ./NvStrapsReBar --restore current.bin --save
```

Restoration checks the recorded GPU/bridge addresses against current hardware.
If the hardware layout changed, reconfigure using the menu. The backup is left
in place if a write fails.

## Firmware and bootloader compatibility

This port implements the exact v0.3 two-byte header, little-endian records,
eight-GPU limit, upstream GPU size registry, and selector precedence. Upstream
v0.4 uses an incompatible eleven-byte header. Do not use this build with a
v0.4 driver. There is no driver-version query in the v0.3 protocol; the tool
rejects malformed or unexpected configuration lengths instead of converting
them automatically.

Before saving, it refreshes GPU locations, BAR0 ranges, and upstream bridges
from IOKit. Like upstream v0.3, it requires a naturally aligned BAR0 below
4 GiB. It refuses an enable operation when no selected NVIDIA GPU is visible.
It also refuses to silently drop active records for a missing or moved GPU.
If a bootloader or ACPI modification hides the NVIDIA GPU from macOS, hardware
discovery must be restored before it can be configured here.

The firmware settings required by upstream still apply: enable ReBAR in UEFI
Setup, or enable Above 4G Decoding and disable CSM on older boards. Disable
NvStrapsReBar before hardware/UEFI setup changes, following the upstream v0.3
release instructions.

On OpenCore systems, the driver's custom GUID must be exposed for NVRAM access.
If your setup uses `NVRAM -> LegacySchema` filtering, include the two variable
names `NvStrapsReBar` and `NvStrapsReBarStatus` under
`E3EE4A27-E2A2-4435-BBA3-184CCAD935A8`. Bootloader-managed emulated NVRAM must
also persist this namespace across reboots. A successful readback verifies the
current macOS view; persistence and the next boot's firmware behavior need to
be checked after reboot. The utility reports permission and sync errors from
macOS and does not change bootloader settings or SIP.

## Build and check

With Apple's Command Line Tools installed:

```sh
sh ReBarState/macos/build.sh --test
```

Outputs are in `build/macos`: the native `NvStrapsReBar` executable, the
double-click launcher, and `NvStrapsReBar-macOS-v0.3.zip`. The executable is
locally ad-hoc signed, not Developer ID signed or notarized.
Packaging downloads the pinned 13,628-byte upstream v0.3 driver if a verified
copy is not already in the build directory; the first package build needs
internet access.

The tests check the v0.3 binary format against independently specified bytes,
truncated/newer-format payloads, selector precedence, BAR and bridge validation,
Intel PCI property decoding, and malformed input under AddressSanitizer and
UndefinedBehaviorSanitizer. They never write firmware variables.

CMake 3.28 or later is also supported:

```sh
cmake -S ReBarState -B build/macos-cmake -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15
cmake --build build/macos-cmake
ctest --test-dir build/macos-cmake --output-on-failure
```
