# meridian-woa

**English** · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md)

A graphical dual-boot menu for **Windows on Arm** laptops: pick Windows 11 or Linux
at power-on, OpenCore-style, on Snapdragon X machines.

`meridian-woa` is a fork of [**Meridian**](https://github.com/seuros/meridian), the
UEFI boot manager by [seuros](https://github.com/seuros). Meridian does all the
real work. This fork adds what it takes to make it boot on Qualcomm ARM64 firmware,
plus a native-resolution theme and installer tooling for a Windows 11 + Omarchy
setup.

![Obsidian on a Yoga Slim 7x (illustration)](docs/media/yoga-slim-7x-obsidian.png)

<sub>Illustration: a laptop in the style of the Lenovo Yoga Slim 7x. The screen content is a real
frame from the renderer at the panel's native 2944×1840, not a drawing.</sub>

## Status

| | |
| --- | --- |
| Tested on | Lenovo Yoga Slim 7x 14Q8X9 (83ED), Snapdragon X Elite, 2944×1840 panel |
| Systems | Windows 11 ARM64 + [Omarchy](https://omarchy.org) (Arch Linux, Limine, LUKS + Btrfs) |
| Firmware | Secure Boot off (the build is unsigned) |
| Other Snapdragon X laptops | Likely to work, untested. Reports welcome |

## Screenshots

In the spirit of upstream: **nothing here is a mockup** (except the laptop around
it). Meridian's Conn renderer builds as an ordinary host program, renders a mock
scan and writes the framebuffer to PNG. The same code draws it on hardware over GOP.

**Obsidian** is the default theme of this fork, drawn at the panel's native resolution:

| Windows 11 selected | Omarchy selected |
| --- | --- |
| ![Obsidian, Windows selected](docs/media/obsidian-windows-1472.png) | ![Obsidian, Omarchy selected](docs/media/obsidian-omarchy-1472.png) |

Full-size frames: [Windows](docs/media/obsidian-windows-2944.png) ·
[Omarchy](docs/media/obsidian-omarchy-2944.png) (2944×1840).

Meridian's own three themes are all still there; press `T` at the menu to cycle
through ATARI, PDP and C64 (screenshots from upstream):

| ATARI | PDP | C64 |
| --- | --- | --- |
| ![ATARI](docs/media/conn-atari.png) | ![PDP](docs/media/conn-pdp.png) | ![C64](docs/media/conn-c64.png) |

## What this fork changes

### 1. Boot loop on ARM64 firmware (the real fix)

Stock Meridian built for AArch64 reset the Yoga Slim 7x straight back to the
vendor logo, forever. The cause is `MrdDetectBootCause()` in
`platform/reset_reason.c`. On non-Apple machines it looks for a coreboot table by
reading **physical addresses `0x0`–`0x1000` and `0xF0000`–`0x100000`**, then falls back
to reading ACPI PM1 through an **x86 I/O port**. Both are PC-BIOS conventions. ARM64
UEFI keeps page 0 unmapped (null-pointer protection) and has no port I/O, so the
first read raises a synchronous exception and the Qualcomm firmware resets the
machine.

On AArch64 those probes are compiled out and the boot cause is reported as
unknown. It only fed a log line, so nothing else changes.

We found it with on-screen and on-ESP breadcrumbs: on this firmware, NVRAM writes
an EFI application makes before the OS starts did **not** survive the reset, so
variable-based tracing showed nothing. A minimal probe image proved the firmware
loads and starts the binary fine (`LoadImage`/`StartImage` = `EFI_SUCCESS`). Two
runs both stopped at the same step.

### 2. Obsidian: native-resolution rendering

Conn draws every theme on a fixed 1104×723 canvas and bilinearly upscales it, about
2.5× on a 2944×1840 panel, so text came out soft. Obsidian now draws straight onto a
surface at the GOP's own resolution and blits it 1:1:

- the layout is authored on the 1104×723 design canvas and scaled to the panel, with no pillarbox bars
- Inter is baked at 1×, 1.5×, 2×, 2.5× and 3× (`conn/tools/bake_obsidian_font.py`); the nearest set is picked at runtime
- anti-aliased rounded corners; pointer hit-testing in native pixels
- the 80×25 retro themes keep upstream's canvas and scaler

Measured on the reference laptop, the redraw is smooth at 2944×1840. The EFI grows
to about 3.3 MB because of the larger glyph sets.

### 3. Firmware hand-off instead of cross-ESP chain-loading

Windows and Omarchy each keep their own ESP. The two menu entries don't chain-load
across partitions; they hand off to the firmware entries that already boot
(`firmware_bootnum`): Windows Boot Manager and Limine. Limine is set back to
`quiet` + `timeout: 1`, so picking Omarchy goes straight in without a second menu,
and pressing a key during that second still opens Limine's snapshot menu.

### 4. Tooling for the Windows 11 + Omarchy setup

| File | Runs on | What it does |
| --- | --- | --- |
| `woa/Meridian-Boot.ps1` | Windows (admin) | `Install` (one-shot `BootNext` test, `BootOrder` untouched), `Promote` / `Demote`, `Status`, `Remove`, `Tidy` (restore a quiet Limine) |
| `woa/meridian-guard.sh` | Omarchy (`sudo`) | systemd unit that puts *Meridian Obsidian* back first in `BootOrder` at every boot and shutdown, because Limine/`efibootmgr` updates otherwise reclaim the default |
| `woa/config.conf` | ESP | Two entries, Windows 11 and Omarchy, 8 s timeout, tools hidden (`showtools none`) |

## Install

> [!WARNING]
> This changes how your laptop boots. Read [Rollback](#rollback) first, keep a Windows
> recovery drive, and make sure you have your BitLocker recovery key.

1. **Requirements:** Secure Boot **off**; Windows and Linux each booting fine on their own.
2. **Get `Meridian.efi`:** download it from Releases or [build](#build) it, and put
   it at `woa\Meridian.efi`. The installer finds both ESPs (by `bootmgfw.efi` and
   `limine.conf`) and the Windows Boot Manager / Limine `Boot####` numbers itself.
   Check what it found with `-Action Status`; override with `-WindowsEsp`, `-LinuxEsp`,
   `-WindowsBootNum`, `-LinuxBootNum` if needed.
3. **Test, one boot only.** In an elevated PowerShell:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\woa\Meridian-Boot.ps1 -Action Install
   ```
   Reboot and pick **Windows 11**. Run `Install` again, reboot and pick **Omarchy**.
   If anything goes wrong, the next power-on falls back to your old boot order.
4. **Make it the default** once both systems have booted through it:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\woa\Meridian-Boot.ps1 -Action Promote
   powershell -ExecutionPolicy Bypass -File .\woa\Meridian-Boot.ps1 -Action Tidy
   ```
5. **Inside Omarchy, once:**
   ```bash
   sudo bash /boot/meridian-guard.sh
   ```

## Rollback

- At power-on, press **F12** (Lenovo boot menu) and pick *Windows Boot Manager* or *Limine*.
- `Meridian-Boot.ps1 -Action Demote` restores the previous `BootOrder`; `-Action Remove`
  deletes the entry and `\EFI\Meridian`. The Windows and Limine loaders are never modified.
- In Omarchy: `sudo bash /boot/meridian-guard.sh --remove`.

## Build

Meridian builds as an EDK2 package; see [README.upstream.md](README.upstream.md).
This fork was built natively on Windows 11 ARM64 with
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (`CLANGDWARF`, `AARCH64`,
`RELEASE`). Windows-host notes:

- `edk2/Conf/build_rule.txt`: call `Trim` as `"$(PYTHON_COMMAND)" ".../Trim/Trim.py"`;
  `make` on this host could not resolve the `Trim.bat` wrapper.
- Screenshots come from the host renderer: `make -C conn render-obsidian W=2944 H=1840`
  (`make -C conn obsidiantest` runs its self-test).

## Repository layout

| Path | Contents |
| --- | --- |
| *(root)* | Meridian source tree with this fork's changes (GPL-3.0-or-later) |
| `woa/` | Windows installer, example config, Omarchy guard |
| `docs/media/` | Screenshots and the README illustration |
| `docs/tools/laptop_mockup.py` | Draws the illustration around a real render |
| `README.upstream.md` | Meridian's original README |

## Credits and license

- **[Meridian](https://github.com/seuros/meridian)** by Abdelkader Boudih (seuros):
  the boot manager, the Conn renderer and the ATARI/PDP/C64 themes. Meridian descends
  from [RefindPlus](https://github.com/dakanji/RefindPlus) (Dayo Akanji) and
  [rEFInd](https://www.rodsbooks.com/refind/) (Roderick W. Smith); see `CREDITS.txt`.
- [Inter](https://rsms.me/inter/) typeface, SIL Open Font License 1.1 (`conn/art/fonts/OFL.txt`).
- [Limine](https://github.com/limine-bootloader/limine) and [Omarchy](https://omarchy.org)
  on the Linux side.

`meridian-woa` is licensed **GPL-3.0-or-later**, like Meridian (`LICENSE.txt`).
The ARM64 boot-cause fix is general and will be proposed upstream.

Not affiliated with or endorsed by Lenovo, Qualcomm or Microsoft. Yoga, Snapdragon
and Windows are trademarks of their respective owners. Use at your own risk.
