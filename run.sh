#!/bin/bash
# ============================================================
#  run.sh - Build and run the 64-bit PC11 bootloader + kernel.
#
#  Just run:   ./run.sh
#  It assembles the long-mode bootloader and the 64-bit kernel,
#  builds the disk image (sector 1 = bootloader, sectors 2+ =
#  kernel), and boots it in QEMU.
# ============================================================
set -e
cd "$(dirname "$0")"

echo "[*] Assembling 64-bit bootloader (boot.asm -> boot.com)..."
nasm -f bin boot.asm -o boot.com

echo "[*] Assembling 64-bit kernel (pc11/os.asm -> os.img)..."
nasm -f bin -o os.img pc11/os.asm

echo "[*] Building disk image (disk.img)..."
dd if=/dev/zero of=disk.img bs=512 count=2880 2>/dev/null
dd if=boot.com of=disk.img bs=512 seek=0 conv=notrunc 2>/dev/null   # sector 1 = bootloader
dd if=os.img   of=disk.img bs=512 seek=1 conv=notrunc 2>/dev/null   # sectors 2+ = kernel

echo "[*] Booting in QEMU (close the window to exit)..."
qemu-system-x86_64 -fda disk.img
