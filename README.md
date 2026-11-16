# NvStrapsReBar-MacOS

A native **Intel macOS** version of the configuration utility from
[terminatorul/NvStrapsReBar v0.3](https://github.com/terminatorul/NvStrapsReBar/releases/tag/v0.3).
Configure GPU and PCI BAR sizes from macOS without booting Windows.

[Download the macOS release](https://github.com/HighDelay/NvStrapsReBar-MacOS/releases/latest)

The release ZIP includes:

- `NvStrapsReBar`: the native macOS Terminal utility.
- `NvStrapsReBar.command`: a double-click Terminal launcher.
- `NvStrapsReBar.ffs`: the **original, unmodified upstream v0.3 UEFI driver**.
- Documentation, the MIT license, SHA-256 checksums, and driver provenance.

## Run

Extract the ZIP and double-click `NvStrapsReBar.command`. Enter your macOS
password when `sudo` asks for it. In the menu, `E` toggles automatic Turing GPU
sizes, `P` selects the target PCI BAR size, and `S` saves. `V` previews changes;
`Q` quits. Changes stay in memory until you save.

For a read-only view from Terminal:

```sh
./NvStrapsReBar --show
```

Command-line configuration is a preview until you include `--save`:

```sh
./NvStrapsReBar --enable --pci-size 64
sudo ./NvStrapsReBar --enable --pci-size 64 --save
```

Use the settings appropriate to your motherboard. Boards with working native
ReBAR support generally use PCI selector `0` (system default); older boards
may need `64` (configure selected GPUs). See the
[macOS documentation](ReBarState/macos/README.md) for all selectors, per-GPU
settings, backups, restoration, and bootloader requirements.

## Requirements

- Intel macOS 10.15 or later and an NVIDIA GPU visible to IOKit.
- The upstream **v0.3** `NvStrapsReBar.ffs` driver installed in motherboard
  firmware, using the [upstream installation instructions](https://github.com/terminatorul/NvStrapsReBar/tree/v0.3#updating-uefi-image).
- Working access to the driver's EFI variables. OpenCore/emulated NVRAM setups
  must expose and persist the driver's custom GUID.

A reboot is still required after saving. The utility does not flash firmware
or install a GPU driver. It targets the **v0.3 firmware format**; upstream
v0.4 uses an incompatible format. Apple Silicon is not supported.

Follow upstream's firmware prerequisites: enable ReBAR in UEFI Setup, or enable
Above 4G Decoding and disable CSM on older boards. Disable NvStrapsReBar before
hardware or UEFI setup changes.

Every save makes a binary backup, checks GPU/bridge addresses, and verifies
firmware readback. Readback does not verify persistence across reboot.

## Build

With Apple's Command Line Tools installed:

```sh
sh ReBarState/macos/build.sh --test
```

The native executable, launcher, original upstream driver, and release ZIP
are written to `build/macos`. The first package build downloads and verifies
the original v0.3 driver. Tests check binary compatibility, selector precedence,
hardware validation, and malformed inputs under AddressSanitizer and
UndefinedBehaviorSanitizer without writing firmware variables.

The executable is ad-hoc signed; it is not Developer ID signed or notarized.
CMake instructions and implementation details are in the
[macOS README](ReBarState/macos/README.md).

## Credits and license

The firmware driver, GPU registry, and Windows tool are from
[terminatorul/NvStrapsReBar](https://github.com/terminatorul/NvStrapsReBar),
based on [xCuri0/ReBarUEFI](https://github.com/xCuri0/ReBarUEFI) and the NVIDIA
register research documented by [envytools](https://github.com/envytools/envytools).
The bundled `.ffs` is the upstream release asset, not a rebuild. Its SHA-256
and original download URL are recorded in
[UPSTREAM-DRIVER.txt](ReBarState/macos/UPSTREAM-DRIVER.txt).

[MIT license](LICENSE).
