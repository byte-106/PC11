![alt text](PC11/logo.png)

# PC11 — a tiny experimental OS (v0.2)

PC11 is a small, from-scratch operating system for modern PCs. It boots
via **UEFI** into **64-bit (x86-64)** long mode and runs a graphical
**window manager** with several built-in apps — everything is
software-rendered to the UEFI GOP framebuffer, with input read directly
from the PS/2 mouse and the UEFI keyboard. It builds into a single
bootable **`pc11.iso`** that runs in QEMU or on real UEFI hardware.

## Features

### System
- Boots as a UEFI application (`BOOTX64.EFI`) — no BIOS, no legacy bootloader.
- 64-bit graphics drawn straight to the UEFI **GOP** linear framebuffer.
- Requests a **1920×1080** display mode at startup (falls back to the
  firmware's current mode if 1080p isn't available). The whole UI is
  resolution-independent — it reads the actual framebuffer size at boot.
- Ships as a single **bootable hybrid ISO** (`pc11.iso`) — works in QEMU and,
  written to a USB stick (`dd if=pc11.iso of=/dev/sdX`), on real PCs.
- **Crash screen**: a CPU exception handler (custom IDT) catches faults,
  shows `PC11 has crashed :(` with the **error number + name + error code**,
  counts down **5 seconds**, then automatically reboots — instead of a
  silent triple-fault reboot.
- The UEFI **watchdog timer is disabled** at startup, so PC11 keeps running
  indefinitely (the firmware would otherwise force-reboot after ~5 minutes).

### Desktop & window manager
- Desktop with a bottom **taskbar**: a **Start button** showing the PC11
  **icon**, one entry per open window, and a live **clock**.
- The Start-button **icon** is a real PNG (`PC11/logo.png`) decoded at boot
  and smoothly area-scaled (alpha-weighted, anti-aliased) into the button.
- **Start menu** pops up from the taskbar listing every app; **Power Off**
  is pinned at the bottom, below a separator.
- **Windows** have **rounded corners** and **anti-aliased text**. You can:
  - move them (drag the title bar),
  - **resize** them (drag the bottom-right grip),
  - **minimize** `[-]` and **close** `[x]` via title-bar buttons,
  - focus/raise by clicking (proper z-order),
  - restore/raise from the taskbar.
- A custom desktop **wallpaper** can be set from an image (see Files).

### Apps
- **About PC11** — info about the system.
- **Files** — a *real* file manager:
  - enumerates the actual disk **volumes** (drives),
  - browses real folders, descends into subdirectories, goes up with
    `[..]` / the **Up** button,
  - previews file contents (text-wrapped inside the window),
  - **New** creates and **Del** deletes real files,
  - reads/writes the live filesystem via UEFI (boot services stay alive).
  - **Right-click** a file (or the desktop) for a context menu:
    **Copy**, **Edit**, **Set as BG** (images only), **Paste**.
  - **Copy/Paste** duplicates real files: copy a file, navigate elsewhere,
    then paste to write a real copy there.
  - **Set as Background** decodes an image and uses it as the wallpaper.
    Formats, all via **from-scratch decoders**: **PNG** (8-bit RGB/RGBA,
    non-interlaced), **JPG/JPEG** (baseline sequential), **BMP** (24/32-bit).
- **Editor** — a text editor that:
  - **Open** button lists files in the current folder to pick one,
  - or open a file from the Files right-click → **Edit**,
  - edit the text, and **Save** writes it back to the real disk.
- **Terminal** — a command shell: `help`, `ver`, `echo`, `about`, `clear`.
- **Calculator** — a working calculator with a clickable keypad
  (digits, `+ - * /`, `C`, `=`).
- **Clock** — shows the current time and date from the hardware RTC.
- **Power Off** — **Shut Down** and **Restart** (UEFI Runtime
  Services `ResetSystem`).

### Input
- **Mouse** via direct **PS/2** port polling (left + right buttons).
- **Keyboard** via the UEFI console (`ConIn`).
- (Boot services are kept alive so the file manager can read real disks
  on demand; the PS/2 mouse is polled directly because the UEFI pointer
  protocol is unreliable under OVMF.)

### Performance
Everything is software-rendered, so the renderer is tuned to stay responsive
even at 1080p:
- The scaled **wallpaper is cached** (`DESK` buffer) and only rebuilt when
  the wallpaper or resolution changes — moving the mouse no longer re-scales
  ~2 million pixels every frame.
- The desktop copy and the back-buffer→framebuffer **blit use bulk
  (word-at-a-time) `memcpy`** instead of scalar per-pixel loops.
- The renderer only redraws on actual input/activity and idles otherwise.
- `run.sh` compiles with **`-O2`**, which substantially speeds up the
  pixel-pushing loops (fonts, windows, icon, scaling).

## Files

| File             | Purpose                                                  |
|------------------|----------------------------------------------------------|
| `PC11/wm.c`       | The window manager and all apps.                         |
| `PC11/efi.h`      | Minimal freestanding UEFI definitions.                   |
| `PC11/font8x8.h`  | 8x8 bitmap font (ASCII), drawn anti-aliased.             |
| `PC11/logo.png`   | The PC11 Start-button icon (decoded + scaled at boot).   |
| `PC11/png.h`      | From-scratch PNG (+ DEFLATE/inflate) decoder.            |
| `PC11/jpg.h`      | From-scratch baseline JPEG decoder.                      |
| `PC11/crash.h`    | CPU exception handling for the crash screen.             |
| `run.sh`         | Builds `pc11.iso` and boots it in QEMU + OVMF.           |
| `home/`          | Files placed on the disk, shown in the Files app as `/home` (e.g. `home/documents`, `home/pictures`). |

## Build & run

You need a PE/EFI-capable compiler, **mtools**, **xorriso**, **QEMU**, and
**OVMF** (UEFI) firmware. `run.sh` is a bash script that auto-detects the
toolchain and firmware and prints the exact install command if something
is missing.

**Linux**

- Arch: `sudo pacman -S --needed mingw-w64-gcc mtools libisoburn qemu-desktop edk2-ovmf`
- Debian/Ubuntu: `sudo apt-get install -y gcc-mingw-w64-x86-64 mtools xorriso qemu-system-x86 ovmf`
- Fedora: `sudo dnf install -y mingw64-gcc mtools xorriso qemu-system-x86 edk2-ovmf`

**macOS** (Homebrew)

```sh
brew install mingw-w64 mtools xorriso qemu
```

**Windows**

Run the script from a Unix-like shell. Two options:

1. **WSL** (recommended) — install Ubuntu from the Microsoft Store, then
   follow the Debian/Ubuntu line above and run `./run.sh` inside WSL.
2. **MSYS2** — install [MSYS2](https://www.msys2.org/), open the
   *MSYS2 MinGW x64* shell, then:
   ```sh
   pacman -S --needed mingw-w64-x86_64-gcc mtools xorriso
   ```
   Install **QEMU for Windows** (https://qemu.weilnetz.de/w64/) — it bundles
   the OVMF firmware (`edk2-x86_64-code.fd`). `run.sh` automatically adds the
   default install dirs (e.g. `C:\Program Files\qemu`) to its search path and
   finds both the `qemu-system-x86_64.exe` binary and the bundled firmware.

**Then, on any platform:**
```sh
./run.sh
```
`run.sh` compiles `PC11/wm.c`, builds a FAT EFI System Partition, wraps it
into a single bootable **`pc11.iso`**, finds your OVMF firmware
automatically, and boots the ISO under QEMU. All intermediate files are
created in a temp dir and removed — the only output is `pc11.iso`.

To put it on real hardware (⚠️ erases the target):
```sh
# Linux/macOS:
sudo dd if=pc11.iso of=/dev/sdX bs=4M status=progress
# Windows: write pc11.iso to a USB stick with Rufus or balenaEtcher.
```

## Controls

- **Start button** (bottom-left, the PC11 icon): open the Start menu, then
  click an app to launch it. Power Off is pinned at the bottom of the menu.
- **Mouse**: move the cursor; **left-click** a window to focus/raise it;
  drag the title bar to move; drag the bottom-right grip to resize; click
  `[-]` to minimize and `[x]` to close. **Right-click** a file (or the
  desktop) for the Copy / Edit / Set-as-BG / Paste context menu.
- **Taskbar**: click an entry to restore/raise that window.
- **Keyboard**: `Tab` cycles focus; arrow keys move the focused window (or
  navigate the Files list); type into the Terminal/Editor when focused;
  `Esc` clears the screen and halts.

## How it works

PC11 uses **UEFI**: the firmware (OVMF under QEMU) loads
`EFI/BOOT/BOOTX64.EFI` already in 64-bit mode and hands us a graphics
framebuffer via the **Graphics Output Protocol (GOP)**.

At startup PC11:
1. Disables the UEFI **watchdog timer** (otherwise the firmware reboots
   after ~5 minutes).
2. Installs a small **IDT** (copying the firmware's, overlaying only the
   CPU-exception vectors) so faults reach the crash screen while normal
   interrupts keep working.
3. Locates the GOP framebuffer, selects a **1920×1080** mode if available
   (otherwise keeps the current one), and allocates a back buffer plus a
   cached-desktop buffer.
4. **Keeps UEFI boot services alive** so the Files app can read real disks
   on demand (Simple File System + File protocols).
5. Reads the **keyboard** through the UEFI console (`ConIn`) and the
   **mouse** by polling the **PS/2 controller** (i8042, ports `0x60`/`0x64`).
6. Runs the window-manager loop: copy the **cached desktop** (the wallpaper
   is scaled only when it changes), draw windows and the cursor into the back
   buffer, then bulk-`memcpy` it to the framebuffer.

Power off / restart use **UEFI Runtime Services** (`ResetSystem`).

## Status

Experimental, v0.2 — a learning/hobby OS, not a general-purpose system.
Image support is limited to common cases (8-bit PNG, baseline JPEG,
uncompressed BMP); the disk filesystem is whatever UEFI exposes (FAT).
