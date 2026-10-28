# meridian-woa

[English](README.md) · **简体中文** · [繁體中文](README.zh-TW.md)

给 **Windows on Arm** 笔记本用的图形化双系统引导菜单：在骁龙 X 机器上开机时选择进入 Windows 11 或 Linux，体验类似 OpenCore。

`meridian-woa` 是 [**Meridian**](https://github.com/seuros/meridian) 的分支。Meridian 是 [seuros](https://github.com/seuros) 开发的 UEFI 引导管理器，真正的核心工作都是它完成的。本分支做的事是：让它能在高通 ARM64 固件上正常启动，加上一套按屏幕原生分辨率绘制的主题，以及为 Windows 11 + Omarchy 双系统准备的安装脚本。

![Obsidian 运行在 Yoga Slim 7x 上（示意图）](docs/media/yoga-slim-7x-obsidian.png)

<sub>示意图：参照联想 Yoga Slim 7x 外观绘制的笔记本。屏幕内容是渲染器按屏幕原生分辨率 2944×1840 输出的真实画面，不是手绘的。</sub>

## 状态

| | |
| --- | --- |
| 测试机型 | 联想 Yoga Slim 7x 14Q8X9（83ED），骁龙 X Elite，2944×1840 屏幕 |
| 系统 | Windows 11 ARM64 + [Omarchy](https://omarchy.org)（Arch Linux，Limine，LUKS + Btrfs） |
| 固件 | 需关闭安全启动（Secure Boot），本构建未签名 |
| 其他骁龙 X 笔记本 | 大概率可用，但未测试，欢迎反馈 |

## 截图

沿用上游的做法：**这里的截图都不是效果图**（只有外面那台笔记本是画的）。Meridian 的 Conn 渲染器可以编译成普通的电脑程序，用模拟的扫描结果渲染一帧，再把帧缓冲区保存成 PNG。实机上用的是同一套代码，通过 GOP 画到屏幕上。

**Obsidian** 是本分支的默认主题，按屏幕原生分辨率绘制：

| 选中 Windows 11 | 选中 Omarchy |
| --- | --- |
| ![Obsidian，选中 Windows](docs/media/obsidian-windows-1472.png) | ![Obsidian，选中 Omarchy](docs/media/obsidian-omarchy-1472.png) |

原尺寸画面：[Windows](docs/media/obsidian-windows-2944.png) · [Omarchy](docs/media/obsidian-omarchy-2944.png)（2944×1840）。

Meridian 自带的三个主题都还在。在菜单里按 `T` 可以在 ATARI、PDP、C64 之间切换（截图来自上游）：

| ATARI | PDP | C64 |
| --- | --- | --- |
| ![ATARI](docs/media/conn-atari.png) | ![PDP](docs/media/conn-pdp.png) | ![C64](docs/media/conn-c64.png) |

## 本分支改了什么

### 1. ARM64 固件上的循环重启（真正的修复）

原版 Meridian 编译成 AArch64 后，在 Yoga Slim 7x 上一启动就回到厂商 logo，无限循环。原因在 `platform/reset_reason.c` 的 `MrdDetectBootCause()`：在非 Apple 机器上，它会读取**物理地址 `0x0`–`0x1000` 和 `0xF0000`–`0x100000`** 来寻找 coreboot 表，找不到再通过 **x86 的 I/O 端口**读取 ACPI PM1。这两种做法都是 PC BIOS 时代的约定。ARM64 的 UEFI 不映射第 0 页（用来拦截空指针），也没有端口 I/O，所以第一次读取就触发同步异常，高通固件随即复位整机。

在 AArch64 上，这些探测代码不再编译，开机原因记为"未知"。这个值原本只用来写一行日志，所以对其他功能没有影响。

定位方法是把每一步的进度显示在屏幕上，同时写进 ESP 上的文件。在这台机器的固件上，EFI 程序在系统启动之前写入的 NVRAM 变量重启后**没有保存下来**，所以用变量记录进度什么也看不到。另外用一个最小的探针程序确认了固件能正常加载并启动这个文件（`LoadImage` 和 `StartImage` 都返回 `EFI_SUCCESS`）。两次实机测试都停在同一步。

### 2. Obsidian：按原生分辨率绘制

Conn 的所有主题都画在固定的 1104×723 画布上，再用双线性插值放大。在 2944×1840 的屏幕上相当于放大约 2.5 倍，文字会发虚。现在 Obsidian 直接按 GOP 报告的分辨率绘制，然后 1:1 显示到屏幕上：

- 布局仍以 1104×723 为设计稿，按屏幕尺寸缩放，左右不再有黑边
- Inter 字体按 1×、1.5×、2×、2.5×、3× 五档烘焙（`conn/tools/bake_obsidian_font.py`），运行时自动选择最接近的一档
- 圆角带抗锯齿；鼠标点击按屏幕实际像素判断
- 80×25 的复古主题仍使用上游的画布和缩放方式

在测试机上实测，2944×1840 下刷新流畅。因为加了大字号字形，EFI 文件增大到约 3.3 MB。

### 3. 交给固件启动项，而不是跨 ESP 链式加载

Windows 和 Omarchy 各自保留自己的 ESP。两个菜单项不直接跨分区加载，而是交给固件里本来就能正常启动的启动项（`firmware_bootnum`）：Windows Boot Manager 和 Limine。Limine 恢复为 `quiet` + `timeout: 1`，所以选 Omarchy 后会直接进系统，不会再出现第二个菜单。需要时在那 1 秒内按任意键，仍然可以打开 Limine 的快照菜单。

### 4. 针对 Windows 11 + Omarchy 的工具

| 文件 | 在哪里运行 | 作用 |
| --- | --- | --- |
| `woa/Meridian-Boot.ps1` | Windows（管理员） | `Install`（用一次性的 `BootNext` 测试，不改 `BootOrder`）、`Promote` / `Demote`、`Status`、`Remove`、`Tidy`（恢复安静模式的 Limine） |
| `woa/meridian-guard.sh` | Omarchy（`sudo`） | systemd 服务，每次开机和关机时把 *Meridian Obsidian* 放回 `BootOrder` 第一位，否则 Limine 或 `efibootmgr` 更新后会把默认启动项改回去 |
| `woa/config.conf` | ESP | 只有 Windows 11 和 Omarchy 两项，倒计时 8 秒，隐藏工具项（`showtools none`） |

## 安装

> [!WARNING]
> 这会改变笔记本的启动方式。请先阅读[回滚](#回滚)，准备好 Windows 恢复 U 盘，并确认手上有 BitLocker 恢复密钥。

1. **前提：** 关闭安全启动（Secure Boot）；Windows 和 Linux 各自都能正常启动。
2. **准备 `Meridian.efi`：** 从 Releases 下载或自己[编译](#编译)，放到 `woa\Meridian.efi`。安装脚本会自动找出两个 ESP（根据 `bootmgfw.efi` 和 `limine.conf`），以及 Windows Boot Manager 和 Limine 的 `Boot####` 编号。用 `-Action Status` 可以查看识别结果；需要时可以用 `-WindowsEsp`、`-LinuxEsp`、`-WindowsBootNum`、`-LinuxBootNum` 手动指定。
3. **测试（只影响下一次开机）：** 在管理员 PowerShell 中运行：
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\woa\Meridian-Boot.ps1 -Action Install
   ```
   重启后选 **Windows 11**。再运行一次 `Install`，重启后选 **Omarchy**。如果出了问题，下次开机会按原来的启动顺序启动。
4. **设为默认：** 两个系统都能从它正常启动后运行：
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\woa\Meridian-Boot.ps1 -Action Promote
   powershell -ExecutionPolicy Bypass -File .\woa\Meridian-Boot.ps1 -Action Tidy
   ```
5. **在 Omarchy 里运行一次：**
   ```bash
   sudo bash /boot/meridian-guard.sh
   ```

## 回滚

- 开机时按 **F12**（联想启动菜单），选择 *Windows Boot Manager* 或 *Limine*。
- `Meridian-Boot.ps1 -Action Demote` 恢复原来的 `BootOrder`；`-Action Remove` 删除启动项和 `\EFI\Meridian`。Windows 和 Limine 的引导文件始终不会被修改。
- 在 Omarchy 中：`sudo bash /boot/meridian-guard.sh --remove`。

## 编译

Meridian 是一个 EDK2 包，编译方法见 [README.upstream.md](README.upstream.md)。本分支是在 Windows 11 ARM64 上用 [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 直接编译的（`CLANGDWARF`、`AARCH64`、`RELEASE`）。在 Windows 上编译时要注意：

- `edk2/Conf/build_rule.txt`：把 `Trim` 改成 `"$(PYTHON_COMMAND)" ".../Trim/Trim.py"` 的形式调用。在这台机器上，`make` 找不到 `Trim.bat` 这个包装脚本。
- 截图由电脑端渲染程序生成：`make -C conn render-obsidian W=2944 H=1840`（`make -C conn obsidiantest` 运行它的自检）。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| *（根目录）* | 包含本分支改动的 Meridian 源码（GPL-3.0-or-later） |
| `woa/` | Windows 安装脚本、示例配置、Omarchy 守护脚本 |
| `docs/media/` | 截图和 README 示意图 |
| `docs/tools/laptop_mockup.py` | 在真实渲染画面外绘制示意图的脚本 |
| `README.upstream.md` | Meridian 原版 README |

## 致谢与许可

- **[Meridian](https://github.com/seuros/meridian)**，作者 Abdelkader Boudih（seuros）：引导管理器本体、Conn 渲染器以及 ATARI/PDP/C64 主题。Meridian 源自 [RefindPlus](https://github.com/dakanji/RefindPlus)（Dayo Akanji）和 [rEFInd](https://www.rodsbooks.com/refind/)（Roderick W. Smith），详见 `CREDITS.txt`。
- [Inter](https://rsms.me/inter/) 字体，SIL Open Font License 1.1（`conn/art/fonts/OFL.txt`）。
- Linux 一侧用到了 [Limine](https://github.com/limine-bootloader/limine) 和 [Omarchy](https://omarchy.org)。

`meridian-woa` 与 Meridian 一样采用 **GPL-3.0-or-later** 许可（`LICENSE.txt`）。ARM64 开机原因检测的修复是通用的，会提交给上游。

本项目与联想、高通、微软均无关联，也未获得它们的认可。Yoga、Snapdragon、Windows 是各自所有者的商标。风险自负。
