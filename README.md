# PC11 — a 64-bit bootloader + kernel

A tiny x86-64 system: a boot sector that takes the CPU into **64-bit long
mode** and hands control to a small **64-bit kernel** (PC11) with an
interactive `$` command prompt.

## Files

| File          | Purpose                                                            |
|---------------|-------------------------------------------------------------------|
| `boot.asm`    | The bootloader. Starts in 16-bit real mode (required), then goes 16 → 32 → 64-bit long mode and jumps to the kernel. |
| `pc11/os.asm` | The 64-bit kernel. VGA-text output + PS/2 keyboard input, with a `$` prompt and built-in commands. |
| `pc11/LICENSE`| License from the original bootOS project this kernel is based on.  |
| `run.sh`      | Builds everything and boots it in QEMU.                            |

## Run

```sh
./run.sh
```

This assembles `boot.asm` and `pc11/os.asm`, builds a 1.44 MB floppy image
(`disk.img`) with the bootloader on sector 1 and the kernel on sectors 2+,
and boots it with `qemu-system-x86_64`.

## How it works

A boot sector **must** start in 16-bit real mode, so the early bootstrap is
16-bit. From there the bootloader:

1. Loads the kernel from disk (via BIOS, while still in real mode).
2. Enables the A20 line, loads a GDT, and enters **32-bit protected mode**.
3. Copies the kernel up to physical `0x100000` (1 MiB).
4. Builds identity-mapped page tables (PML4 → PDPT → PD, first 1 GiB).
5. Enables PAE + EFER.LME + paging → enters **64-bit long mode**.
6. Far-jumps into 64-bit code and jumps to the kernel at 1 MiB.

The kernel runs in long mode where **BIOS interrupts no longer exist**, so it
drives hardware directly:

* **Output** — writes to the VGA text buffer at `0xB8000`, with cursor
  tracking, scrolling, and hardware-cursor updates.
* **Input** — reads the PS/2 keyboard (ports `0x60`/`0x64`) and translates
  scancodes to ASCII.

### Built-in commands

```
ver    print the banner
help   list commands
cls    clear the screen
halt   stop the CPU
```

## Note

This kernel is a 64-bit port of the structure of nanochess's bootOS. The
original's disk-backed file commands (`dir`, `format`, `enter`, `del`,
running saved programs) relied on BIOS `int 0x13` and are **not** ported,
since that needs a native 64-bit disk driver. The interactive shell and the
built-in commands above all work.
