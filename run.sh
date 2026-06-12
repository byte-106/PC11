#!/bin/bash
# ============================================================
#  run.sh - Build PC11 into a single bootable ISO and run it.
#
#  Output:  pc11.iso   (the ONLY file produced; works in QEMU
#                        and on real UEFI PCs, like any OS ISO)
#
#  All intermediate files live in a temp dir that is removed
#  afterwards, so your project folder stays clean.
# ============================================================
set -e
cd "$(dirname "$0")"

OUT="pc11.iso"
say()  { printf '\033[1;36m[*]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[!]\033[0m %s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---- detect the host (Windows = MSYS2 / Git-Bash / Cygwin) -----------
WIN=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) WIN=1 ;;
esac
# On Windows, common install dirs aren't always on PATH; add a few.
if [ "$WIN" = 1 ]; then
    for d in "/c/Program Files/qemu" "/c/Program Files/mtools" \
             "/c/msys64/mingw64/bin" "/c/msys64/usr/bin"; do
        [ -d "$d" ] && PATH="$PATH:$d"
    done
fi

# helper: pick the first available command name from a list
pick() { for c in "$@"; do if have "$c"; then echo "$c"; return 0; fi; done; return 1; }

# ---- toolchain checks -------------------------------------------------
CC=""; MODE=""
if   have x86_64-w64-mingw32-gcc; then CC="x86_64-w64-mingw32-gcc"; MODE="mingw"
elif have gcc && [ "$WIN" = 1 ]; then CC="gcc"; MODE="mingw"   # MSYS2 mingw gcc
elif have clang;                  then CC="clang";              MODE="clang"
fi
if [ -z "$CC" ]; then
    err "No EFI compiler (need mingw-w64 gcc or clang)."
    err "  Arch:    sudo pacman -S --needed mingw-w64-gcc"
    err "  Debian:  sudo apt-get install -y gcc-mingw-w64-x86-64"
    err "  Fedora:  sudo dnf install -y mingw64-gcc"
    err "  macOS:   brew install mingw-w64"
    err "  Windows: in MSYS2:  pacman -S mingw-w64-x86_64-gcc"
    exit 1
fi
if ! have mformat || ! have mcopy || ! have mmd; then
    err "mtools (mformat/mcopy/mmd) not found."
    err "  Arch: pacman -S mtools | Debian: apt-get install mtools"
    err "  Windows (MSYS2): pacman -S mtools"
    exit 1
fi
if ! have xorriso; then
    err "xorriso not found."
    err "  Arch: pacman -S libisoburn | Debian: apt-get install xorriso"
    err "  Windows (MSYS2): pacman -S xorriso"
    exit 1
fi
# QEMU binary may be named with .exe on Windows
QEMU="$(pick qemu-system-x86_64 qemu-system-x86_64.exe)"

# ---- temp build dir (cleaned on exit) --------------------------------
BUILD="$(mktemp -d)"
cleanup(){ rm -rf "$BUILD"; }
trap cleanup EXIT
EFI="$BUILD/BOOTX64.EFI"; ESP="$BUILD/esp.img"; ISODIR="$BUILD/iso"

# ---- 1) compile the EFI application ----------------------------------
say "Compiling EFI application (PC11/wm.c)..."
if [ "$MODE" = "mingw" ]; then
    "$CC" -I PC11 -O2 \
        -ffreestanding -fno-stack-protector -fno-stack-check \
        -fshort-wchar -mno-red-zone -Wall -Wextra \
        -e efi_main -nostdlib -Wl,--subsystem,10 \
        -o "$EFI" PC11/wm.c
else
    "$CC" -I PC11 -O2 \
        --target=x86_64-unknown-windows \
        -ffreestanding -fno-stack-protector \
        -fshort-wchar -mno-red-zone -Wall -Wextra \
        -nostdlib -fuse-ld=lld \
        -Wl,-entry:efi_main -Wl,-subsystem:efi_application \
        -o "$EFI" PC11/wm.c
fi

# ---- 2) build the FAT EFI System Partition ---------------------------
say "Building EFI System Partition..."
efik=$(( ($(wc -c < "$EFI") + 1023) / 1024 ))
homek=$(du -sk home 2>/dev/null | awk '{print $1}'); [ -z "$homek" ] && homek=0
mb=$(( (efik + homek) / 1024 + 6 ))
[ "$mb" -lt 8 ] && mb=8
dd if=/dev/zero of="$ESP" bs=1M count="$mb" 2>/dev/null
# NOTE: no -F (FAT32) on small images -- FAT32 needs >=~33MB and an undersized
# FAT32 is unmountable by UEFI firmware. Let mformat pick FAT12/16.
mformat -i "$ESP" ::
mmd   -i "$ESP" ::/EFI
mmd   -i "$ESP" ::/EFI/BOOT
mcopy -i "$ESP" "$EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP" "$EFI" ::/BOOTX64.EFI
printf '\\EFI\\BOOT\\BOOTX64.EFI\r\n' > "$BUILD/startup.nsh"
mcopy -i "$ESP" "$BUILD/startup.nsh" ::/startup.nsh
# copy the ./home tree onto the disk (shown as /home in the Files app)
if [ -d home ]; then
    mmd -i "$ESP" ::/home 2>/dev/null || true
    find home -mindepth 1 -type d | sed 's#^home/##' | while read -r d; do
        mmd -i "$ESP" "::/home/$d" 2>/dev/null || true
    done
    find home -type f | while read -r f; do
        rel="${f#home/}"
        mcopy -i "$ESP" "$f" "::/home/$rel" 2>/dev/null || true
    done
fi

# copy the start-menu logo (loaded at boot from /PC11/logo.png)
if [ -f PC11/logo.png ]; then
    mmd   -i "$ESP" ::/PC11 2>/dev/null || true
    mcopy -i "$ESP" PC11/logo.png ::/PC11/logo.png 2>/dev/null || true
fi

# ---- 3) wrap the ESP into a bootable UEFI ISO ------------------------
say "Creating bootable ISO ($OUT)..."
mkdir -p "$ISODIR"
cp "$ESP" "$ISODIR/efiboot.img"
xorriso -as mkisofs \
    -V PC11 -r -J \
    -e efiboot.img -no-emul-boot \
    -isohybrid-gpt-basdat \
    -o "$OUT" "$ISODIR" 2>&1 | grep -iE "^xorriso.*error|fail" || true
say "Done -> $OUT  ($(du -h "$OUT" | cut -f1))"

# ---- 4) boot the ISO in QEMU ----------------------------------------
if [ -z "$QEMU" ]; then
    err "QEMU not found (ISO built at $OUT; install QEMU to run it)."
    err "  Arch: pacman -S qemu-desktop | Debian: apt-get install qemu-system-x86"
    err "  Windows: install QEMU from https://qemu.weilnetz.de/w64/"
    exit 0
fi

# Locate OVMF (UEFI) firmware. Linux/macOS dirs first, then the dirs that
# ship inside a Windows QEMU install (relative to the qemu binary).
OVMF_CODE=""
QEMU_DIR=""
qpath="$(command -v "$QEMU" 2>/dev/null)"
[ -n "$qpath" ] && QEMU_DIR="$(dirname "$qpath")"
for p in \
    /usr/share/edk2/x64/OVMF_CODE.4m.fd \
    /usr/share/edk2/x64/OVMF_CODE.fd \
    /usr/share/edk2-ovmf/x64/OVMF_CODE.fd \
    /usr/share/edk2/ovmf/OVMF_CODE.fd \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/ovmf/x64/OVMF_CODE.fd \
    /usr/share/ovmf/OVMF.fd \
    "$(brew --prefix 2>/dev/null)/share/qemu/edk2-x86_64-code.fd" \
    /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
    /usr/local/share/qemu/edk2-x86_64-code.fd \
    "$QEMU_DIR/share/edk2-x86_64-code.fd" \
    "$QEMU_DIR/share/edk2-i386-code.fd" \
    "$QEMU_DIR/edk2-x86_64-code.fd" \
    "$QEMU_DIR/share/OVMF.fd" \
    "$QEMU_DIR/OVMF.fd" ; do
    [ -n "$p" ] && [ -f "$p" ] && OVMF_CODE="$p" && break
done
if [ -z "$OVMF_CODE" ]; then
    err "OVMF firmware not found (ISO built at $OUT, but can't boot it here)."
    err "  Arch: pacman -S edk2-ovmf | Debian: apt-get install ovmf"
    err "  Windows: QEMU bundles it as edk2-x86_64-code.fd in its install dir."
    exit 0
fi
OVMF_VARS=""; cand="${OVMF_CODE/OVMF_CODE/OVMF_VARS}"; [ -f "$cand" ] && OVMF_VARS="$cand"
# Windows QEMU ships a matching writable vars template too
[ -z "$OVMF_VARS" ] && [ -f "$QEMU_DIR/share/edk2-x86_64-vars.fd" ] && OVMF_VARS="$QEMU_DIR/share/edk2-x86_64-vars.fd"
[ -z "$OVMF_VARS" ] && [ -f "$QEMU_DIR/edk2-x86_64-vars.fd" ]       && OVMF_VARS="$QEMU_DIR/edk2-x86_64-vars.fd"

say "Booting $OUT in QEMU (UEFI). ESC quits the GUI."
COMMON=(-machine q35 -m 256 -cdrom "$OUT" -vga std)
if [ -n "$OVMF_VARS" ]; then
    VARS="$(mktemp)"; cp -f "$OVMF_VARS" "$VARS"
    trap 'rm -rf "$BUILD"; rm -f "$VARS"' EXIT
    "$QEMU" "${COMMON[@]}" \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file="$VARS"
else
    "$QEMU" "${COMMON[@]}" -bios "$OVMF_CODE"
fi
