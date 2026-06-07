; ============================================================
;  boot.asm - A 64-bit (long mode) bootloader
;
;  A boot sector ALWAYS starts in 16-bit real mode, so the
;  early bootstrap below is necessarily 16-bit. Its job is to
;  take the CPU all the way to 64-bit long mode and then jump
;  to the 64-bit kernel.
;
;  Path to long mode:
;     16-bit real mode
;        -> load kernel from disk
;        -> enable A20 (already on under QEMU/BIOS, but we try)
;        -> load GDT, enter 32-bit protected mode
;        -> build identity-mapped page tables (PML4)
;        -> enable PAE + LME + paging  => 64-bit long mode
;        -> far jump into 64-bit code, then jump to the kernel
;
;  Disk layout:
;     sector 1                 = THIS bootloader
;     sectors 2..(1+N)         = the 64-bit kernel (os.img)
;
;  The kernel is loaded to physical 0x100000 (1 MiB).
;
;  Assemble:  nasm -f bin boot.asm -o boot.com
; ============================================================

        bits 16
        org 0x7C00

KERNEL_LBA      equ 1           ; kernel starts at LBA 1 (sector 2, 0-based 1)
KERNEL_SECTORS  equ 16          ; how many 512-byte sectors to load (8 KiB)
KERNEL_PHYS     equ 0x100000    ; load kernel at 1 MiB
KERNEL_TMP_SEG  equ 0x1000      ; temp real-mode buffer 0x1000:0 = 0x10000

; ------------------------------------------------------------
; 16-bit real-mode entry
; ------------------------------------------------------------
start:
        cli
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0x7C00
        mov [boot_drive], dl

        ; --- print a boot message via BIOS (last time we can) ---
        mov si, msg_boot
        call print16

        ; --- load the kernel into a temporary low buffer (0x10000) ---
        ; (Real-mode BIOS can only easily reach < 1 MiB, so we stage
        ;  it low, then copy it up to 1 MiB once we are in 32-bit mode.)
        mov ah, 0x02            ; BIOS read sectors
        mov al, KERNEL_SECTORS
        mov ch, 0               ; cylinder 0
        mov dh, 0               ; head 0
        mov cl, KERNEL_LBA + 1  ; sector (1-based): LBA1 -> sector 2
        mov bx, KERNEL_TMP_SEG
        mov es, bx
        xor bx, bx              ; ES:BX = 0x1000:0000
        mov dl, [boot_drive]
        int 0x13
        jc disk_error
        xor ax, ax
        mov es, ax

        ; --- enable A20 line (fast A20 via port 0x92) ---
        in al, 0x92
        or al, 2
        out 0x92, al

        ; --- load GDT and enter 32-bit protected mode ---
        lgdt [gdt32_desc]
        mov eax, cr0
        or eax, 1               ; set PE (protection enable)
        mov cr0, eax
        jmp CODE32_SEL:protected_mode

disk_error:
        mov si, msg_diskerr
        call print16
.h: hlt
        jmp .h

; ------------------------------------------------------------
; print16: 16-bit BIOS teletype print of DS:SI (null-terminated)
; ------------------------------------------------------------
print16:
        push ax
        mov ah, 0x0E
.l:     lodsb
        or al, al
        jz .d
        int 0x10
        jmp .l
.d:     pop ax
        ret

; ============================================================
;  32-bit protected mode
; ============================================================
        bits 32
protected_mode:
        mov ax, DATA32_SEL
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov fs, ax
        mov gs, ax
        mov esp, 0x9C000        ; temporary 32-bit stack

        ; --- copy kernel from 0x10000 up to 0x100000 (1 MiB) ---
        mov esi, 0x10000
        mov edi, KERNEL_PHYS
        mov ecx, (KERNEL_SECTORS*512)/4
        rep movsd

        ; --- build identity-mapped page tables for long mode ---
        ; Layout (physical):
        ;   PML4 @ 0x1000, PDPT @ 0x2000, PD @ 0x3000
        ;   Identity-map the first 1 GiB using 512 x 2 MiB pages.
        ; Clear PML4, PDPT, PD (3 * 4 KiB).
        mov edi, 0x1000
        xor eax, eax
        mov ecx, (0x4000-0x1000)/4
        rep stosd

        ; PML4[0] -> PDPT
        mov edi, 0x1000
        mov dword [edi], 0x2000 | 0x3      ; present + writable
        ; PDPT[0] -> PD
        mov edi, 0x2000
        mov dword [edi], 0x3000 | 0x3
        ; PD[i] -> 2 MiB page, i = 0..511, flags present|writable|PS(0x80)
        mov edi, 0x3000
        mov eax, 0x00000083                ; addr 0 | present|rw|PS
        mov ecx, 512
.fill_pd:
        mov [edi], eax
        add eax, 0x200000                  ; next 2 MiB
        add edi, 8
        loop .fill_pd

        ; --- enable PAE ---
        mov eax, cr4
        or eax, 1 << 5          ; CR4.PAE
        mov cr4, eax

        ; --- point CR3 at PML4 ---
        mov eax, 0x1000
        mov cr3, eax

        ; --- set LME in EFER MSR ---
        mov ecx, 0xC0000080     ; IA32_EFER
        rdmsr
        or eax, 1 << 8          ; LME
        wrmsr

        ; --- enable paging (and protection) => long mode active ---
        mov eax, cr0
        or eax, 1 << 31         ; PG
        or eax, 1               ; PE (already set)
        mov cr0, eax

        ; --- load 64-bit GDT and far-jump into 64-bit code ---
        lgdt [gdt64_desc]
        jmp CODE64_SEL:long_mode

; ============================================================
;  64-bit long mode
; ============================================================
        bits 64
long_mode:
        mov ax, DATA64_SEL
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov fs, ax
        mov gs, ax
        mov rsp, 0x90000        ; 64-bit stack

        ; jump to the kernel loaded at 1 MiB
        mov rax, KERNEL_PHYS
        jmp rax

; ============================================================
;  Global Descriptor Tables
; ============================================================
        bits 16

; --- 32-bit GDT (null, 32-bit code, 32-bit data) ---
align 8
gdt32:
        dq 0x0000000000000000   ; null
        dq 0x00CF9A000000FFFF   ; 0x08 code: base0 limit4G, 32-bit, exec/read
        dq 0x00CF92000000FFFF   ; 0x10 data: base0 limit4G, 32-bit, read/write
gdt32_end:
CODE32_SEL equ 0x08
DATA32_SEL equ 0x10
gdt32_desc:
        dw gdt32_end - gdt32 - 1
        dd gdt32

; --- 64-bit GDT (null, 64-bit code, 64-bit data) ---
align 8
gdt64:
        dq 0x0000000000000000   ; null
        dq 0x00AF9A000000FFFF   ; 0x08 code: 64-bit (L=1), exec/read
        dq 0x00AF92000000FFFF   ; 0x10 data: read/write
gdt64_end:
CODE64_SEL equ 0x08
DATA64_SEL equ 0x10
gdt64_desc:
        dw gdt64_end - gdt64 - 1
        dd gdt64

; ------------------------------------------------------------
; data
; ------------------------------------------------------------
boot_drive:  db 0
msg_boot:    db "Booting (64-bit)...", 13, 10, 0
msg_diskerr: db "DISK ERROR!", 13, 10, 0

        times 510-($-$$) db 0
        dw 0xAA55
