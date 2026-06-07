; ============================================================
;  os.asm - PC11, ported to 64-bit long mode
;
;  Based on bootOS by Oscar Toledo G. (nanochess), which was a
;  512-byte REAL-MODE operating system driven by BIOS interrupts
;  (int 0x10 video, int 0x16 keyboard, int 0x13 disk).
;
;  In 64-bit long mode those BIOS services do NOT exist, so this
;  port keeps PC11's structure -- the banner, the '$' command
;  prompt, line input and the command dispatch table -- but
;  replaces the three BIOS I/O primitives with native 64-bit
;  hardware drivers:
;
;     output_char  -> writes to the VGA text buffer (0xB8000),
;                     tracks a cursor and scrolls.
;     input_key    -> reads the PS/2 keyboard (ports 0x60/0x64)
;                     and translates scancodes to ASCII.
;     (disk file commands from the original are not ported here,
;      because a real 64-bit disk driver is a separate large
;      task; the interactive shell and built-in commands work.)
;
;  Loaded at physical 0x100000 (1 MiB) by boot.asm, entered in
;  64-bit long mode with a valid stack.
;
;  Assemble:  nasm -f bin os.asm -o os.img
; ============================================================

        bits 64
        org 0x100000

VGA      equ 0xB8000
COLS     equ 80
ROWS     equ 25
ATTR     equ 0x07            ; light grey on black (like the original)

; ------------------------------------------------------------
; Kernel entry (cold start) -- mirrors PC11's 'start'
; ------------------------------------------------------------
start:
        ; cursor position (linear char index 0..COLS*ROWS-1)
        mov qword [cursor], 0
        call clear_screen

; PC11's 'ver' command path: print the banner, then drop to the
; command loop (warm start / 'restart').
ver_command:
        mov rsi, intro
        call output_string
        call newline

; ------------------------------------------------------------
; restart: the main command loop (PC11's warm start)
; ------------------------------------------------------------
restart:
        mov al, '$'             ; command prompt character
        call input_line         ; read a line; RSI -> line buffer

        cmp byte [rsi], 0x00    ; empty line?
        je restart

        ; --- dispatch against the command table ---
        ; Table layout per entry:  db len ; <name bytes> ; dq handler
        ; A trailing entry with len=0 marks the end.
        mov rbx, commands       ; RBX walks the table
.next_cmd:
        movzx rcx, byte [rbx]   ; RCX = length of this command name
        test rcx, rcx
        jz .not_found           ; length 0 -> end of table
        lea rdi, [rbx + 1]      ; RDI -> name bytes
        mov rsi, line           ; RSI -> typed line
        push rcx                ; save length for advancing later
        repe cmpsb              ; compare name vs line
        jne .skip               ; mismatch
        ; matched the name; require the next typed char to be end/space
        mov al, [rsi]
        cmp al, 0
        je .match
        cmp al, ' '
        je .match
.skip:
        pop rcx                 ; restore name length
        lea rbx, [rbx + 1]      ; skip the length byte
        add rbx, rcx            ; skip the name
        add rbx, 8              ; skip the 8-byte handler pointer
        jmp .next_cmd
.match:
        pop rcx                 ; (discard saved length)
        ; RDI now points just past the name = handler pointer
        call qword [rdi]
        jmp restart

.not_found:
        mov rsi, msg_unknown
        call output_string
        call newline
        jmp restart

; ============================================================
;  Built-in commands
; ============================================================

; ver : print the banner again
do_ver:
        mov rsi, intro
        call output_string
        call newline
        ret

; help : list commands
do_help:
        mov rsi, msg_help
        call output_string
        call newline
        ret

; cls : clear the screen
do_cls:
        call clear_screen
        ret

; halt : stop the CPU
do_halt:
        mov rsi, msg_halt
        call output_string
.h:     hlt
        jmp .h

; ============================================================
;  64-bit I/O drivers (replacing BIOS int 0x10 / 0x16)
; ============================================================

; ------------------------------------------------------------
; clear_screen: fill the VGA buffer with spaces, reset cursor
; ------------------------------------------------------------
clear_screen:
        push rax
        push rcx
        push rdi
        mov rdi, VGA
        mov rcx, COLS*ROWS
        mov ax, (ATTR << 8) | ' '
        rep stosw
        mov qword [cursor], 0
        call move_hw_cursor
        pop rdi
        pop rcx
        pop rax
        ret

; ------------------------------------------------------------
; output_char: print AL to the screen (handles CR/LF/backspace),
;              advance cursor, scroll when needed.
; ------------------------------------------------------------
output_char:
        push rax
        push rbx
        push rcx
        push rdx
        push rdi
        push rsi

        cmp al, 13             ; CR -> ignore (LF does the work)
        je .done
        cmp al, 10             ; LF -> newline
        je .nl
        cmp al, 8              ; backspace
        je .bs

        ; normal character: write at cursor
        mov rbx, [cursor]
        mov rdi, VGA
        lea rdi, [rdi + rbx*2]
        mov ah, ATTR
        mov [rdi], ax
        inc qword [cursor]
        jmp .check_scroll

.nl:
        ; move cursor to start of next line
        mov rax, [cursor]
        xor rdx, rdx
        mov rcx, COLS
        div rcx                ; rax = row, rdx = col
        inc rax                ; next row
        mul rcx                ; rax = row*COLS
        mov [cursor], rax
        jmp .check_scroll

.bs:
        cmp qword [cursor], 0
        je .done
        dec qword [cursor]
        mov rbx, [cursor]
        mov rdi, VGA
        lea rdi, [rdi + rbx*2]
        mov ax, (ATTR << 8) | ' '
        mov [rdi], ax
        jmp .done

.check_scroll:
        cmp qword [cursor], COLS*ROWS
        jb .done
        call scroll
.done:
        call move_hw_cursor
        pop rsi
        pop rdi
        pop rdx
        pop rcx
        pop rbx
        pop rax
        ret

; ------------------------------------------------------------
; scroll: move every line up by one, clear last line, fix cursor
; ------------------------------------------------------------
scroll:
        push rax
        push rcx
        push rsi
        push rdi
        ; copy rows 1..24 over rows 0..23
        mov rsi, VGA + (COLS*2)
        mov rdi, VGA
        mov rcx, COLS*(ROWS-1)
        rep movsw
        ; clear the last row
        mov rdi, VGA + (COLS*(ROWS-1)*2)
        mov rcx, COLS
        mov ax, (ATTR << 8) | ' '
        rep stosw
        ; move cursor to start of last line
        mov qword [cursor], COLS*(ROWS-1)
        pop rdi
        pop rsi
        pop rcx
        pop rax
        ret

; ------------------------------------------------------------
; output_string: print null-terminated string at RSI
; ------------------------------------------------------------
output_string:
        push rax
        push rsi
.l:     lodsb
        test al, al
        jz .d
        call output_char
        jmp .l
.d:     pop rsi
        pop rax
        ret

; newline helper
newline:
        push rax
        mov al, 10
        call output_char
        pop rax
        ret

; ------------------------------------------------------------
; move_hw_cursor: update the VGA hardware cursor (ports 0x3D4/5)
; ------------------------------------------------------------
move_hw_cursor:
        push rax
        push rdx
        mov rbx, [cursor]
        ; low byte
        mov dx, 0x3D4
        mov al, 0x0F
        out dx, al
        mov dx, 0x3D5
        mov al, bl
        out dx, al
        ; high byte
        mov dx, 0x3D4
        mov al, 0x0E
        out dx, al
        mov dx, 0x3D5
        mov rax, [cursor]
        shr rax, 8
        out dx, al
        pop rdx
        pop rax
        ret

; ------------------------------------------------------------
; input_key: wait for a key, return ASCII in AL (PS/2 driver)
; ------------------------------------------------------------
input_key:
.wait:
        in al, 0x64            ; status port
        test al, 1             ; output buffer full?
        jz .wait
        in al, 0x60            ; read scancode
        test al, 0x80          ; key release? ignore
        jnz .wait
        movzx rbx, al
        mov al, [scancodes + rbx]
        test al, al            ; unmapped key? ignore
        jz .wait
        ret

; ------------------------------------------------------------
; input_line: read a line into 'line', echoing, until Enter.
;   AL on entry = prompt char. On return RSI -> line buffer.
; ------------------------------------------------------------
input_line:
        push rax
        call output_char        ; echo the prompt
        mov rdi, line
.read:
        call input_key          ; AL = ASCII
        cmp al, 13              ; Enter?
        je .enter
        cmp al, 8               ; Backspace?
        jne .store
        ; backspace: don't go before start of buffer
        cmp rdi, line
        je .read
        dec rdi
        mov al, 8
        call output_char
        jmp .read
.store:
        cmp rdi, line + 127     ; buffer limit
        jae .read
        mov [rdi], al
        inc rdi
        call output_char        ; echo
        jmp .read
.enter:
        mov byte [rdi], 0       ; null-terminate
        call newline
        mov rsi, line
        pop rax
        ret

; ============================================================
;  Data
; ============================================================
intro:       db "PC11 (64-bit)", 0
msg_unknown: db "Bad command", 0
msg_help:    db "Commands: ver  help  cls  halt", 0
msg_halt:    db "Halted.", 0

; --- command table: db len, "name", dq handler ---
align 8
commands:
        db 3
        db "ver"
        dq do_ver
        db 4
        db "help"
        dq do_help
        db 3
        db "cls"
        dq do_cls
        db 4
        db "halt"
        dq do_halt
        db 0                    ; end of table

; --- US scancode set 1 -> ASCII (only the common keys) ---
align 8
scancodes:
        db 0,    27,  '1', '2', '3', '4', '5', '6'   ; 00-07
        db '7', '8', '9', '0', '-', '=', 8,   9      ; 08-0F (08=bksp,09=tab)
        db 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i'    ; 10-17
        db 'o', 'p', '[', ']', 13,  0,   'a', 's'    ; 18-1F (1C=enter,1D=ctrl)
        db 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';'    ; 20-27
        db 39,  '`', 0,   92,  'z', 'x', 'c', 'v'    ; 28-2F (2A=lshift)
        db 'b', 'n', 'm', ',', '.', '/', 0,   '*'    ; 30-37
        db 0,   ' ', 0,   0,   0,   0,   0,   0      ; 38-3F (39=space)
        times 256-($-scancodes) db 0

; --- runtime variables ---
align 8
cursor:  dq 0                  ; linear cursor position
line:    times 128 db 0        ; line input buffer

        ; pad image to a whole number of sectors (16 sectors = 8 KiB,
        ; matching KERNEL_SECTORS in boot.asm; fits in one floppy track)
        times (512*16)-($-$$) db 0
