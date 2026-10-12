; Sector at LBA 30000000h of the large disk test: writes 64 sectors (word n = n) at LBA 30000001h,
; reads them back to another buffer and compares; prints LBA48-OK or LBA48-FAIL. With IDE block
; mode on, both transfers are READ/WRITE MULTIPLE EXT, several blocks each.
bits 16
org 7e00h
        mov ax, 1000h
        mov es, ax
        xor di, di
        xor ax, ax
        mov cx, 16384
fill:   stosw
        inc ax
        loop fill
        mov si, wdap
        mov ax, 4300h
        mov dl, 80h
        int 13h
        jc fail
        mov si, rdap
        mov ah, 42h
        mov dl, 80h
        int 13h
        jc fail
        push ds
        mov ax, 1000h
        mov ds, ax
        mov ax, 2000h
        mov es, ax
        xor si, si
        xor di, di
        mov cx, 16384
        repe cmpsw
        pop ds
        jne fail
        mov si, ok
        jmp print
fail:   mov si, bad
print:  lodsb
        or al, al
        jz $
        mov ah, 0eh
        mov bx, 7
        int 10h
        jmp print
wdap:   db 16, 0
        dw 64, 0, 1000h
        dq 30000001h
rdap:   db 16, 0
        dw 64, 0, 2000h
        dq 30000001h
ok:     db "LBA48-OK", 0
bad:    db "LBA48-FAIL", 0
        times 512 - ($ - $$) db 0
