; DOS program of the PnP BIOS test: finds the $PnP header, lists the system device nodes (00h, 01h), the
; ISA PnP configuration (40h) and the ESCD boards (41h, 42h). It adds a board in virtual slot 20 (43h),
; reads the ESCD back and compares (ESCD-WRITE-OK); the next POST restores its own ESCD. Then the same
; through the 16-bit protected mode entry: nodes, ESCD read through an
; ESCD selector, write refused (PNP-PM-OK). Prints PNP-TEST-DONE at the end.
bits 16
org 100h
        mov ax, 0f000h                  ; $PnP: paragraph aligned in F000, byte checksum over its length
        mov es, ax
        xor di, di
scan:   cmp dword [es:di], 506e5024h
        jne next
        xor cx, cx
        mov cl, [es:di + 5]
        xor al, al
        mov bx, di
sum:    add al, [es:bx]
        inc bx
        loop sum
        or al, al
        jz found
next:   add di, 16
        jnz scan
        mov dx, nopnp
        jmp fail
found:  mov ax, [es:di + 0dh]
        mov [entry], ax
        mov ax, [es:di + 0fh]
        mov [entry + 2], ax
        mov ax, [es:di + 1bh]
        mov [biosds], ax
        mov ax, [es:di + 11h]
        mov [pmentry], ax
        mov eax, [es:di + 13h]          ; BIOS code and data selectors: their bases
        mov bx, gdt + 18h
        call setbase
        mov eax, [es:di + 1dh]
        mov bx, gdt + 20h
        call setbase
        mov ax, cs
        add ax, 1000h
        mov [escdseg], ax
        add ax, 100h
        mov [copyseg], ax

        push word [biosds]              ; 00h: number of nodes, largest node
        push ds
        push word nodemax
        push ds
        push word nodes
        push word 0
        call far [entry]
        add sp, 12
        or ax, ax
        jnz bad
        mov dx, s_nodes
        call puts
        mov al, [nodes]
        call hex8
        mov ax, [nodemax]
        call hex16
        call crlf

        mov byte [handle], 0            ; 01h: every node, until handle FFh
nodeloop:
        push word [biosds]
        push word 1                     ; current configuration
        push ds
        push word nodebuf
        push ds
        push word handle
        push word 1
        call far [entry]
        add sp, 14
        or ax, ax
        jnz bad
        inc byte [count]
        mov si, nodebuf + 3
        call eisa
        cmp byte [handle], 0ffh
        jne nodeloop
        call crlf
        mov al, [count]
        cmp al, [nodes]
        jne bad
        mov dx, s_nodesok
        call puts

        push word [biosds]              ; 40h: ISA PnP configuration
        push ds
        push word isacfg
        push word 40h
        call far [entry]
        add sp, 8
        or ax, ax
        jnz bad
        mov dx, s_isa
        call puts
        mov al, [isacfg + 1]
        call hex8
        mov ax, [isacfg + 2]
        call hex16
        call crlf

        push word [biosds]              ; 41h: ESCD sizes and base
        push ds
        push word nvbase
        push ds
        push word escdsize
        push ds
        push word minsize
        push word 41h
        call far [entry]
        add sp, 16
        or ax, ax
        jnz bad

        mov ax, [escdseg]
        call readescd
        mov es, [escdseg]
        mov dx, s_escd
        call puts
        mov ax, [es:0]
        call hex16
        mov al, [es:8]
        call hex8
        mov si, 12                      ; boards: slot and EISA ID
        xor cx, cx
        mov cl, [es:8]
        xor bp, bp
boards: push cx
        mov al, ' '
        call putc
        mov al, [es:si + 2]
        cmp al, 20
        jne .other
        inc bp
.other: call hex8
        push ds
        push si
        add si, 4
        mov ax, es
        mov ds, ax
        call eisa
        pop si
        pop ds
        add si, [es:si]
        pop cx
        loop boards
        call crlf
        or bp, bp
        jnz bad

addboard:                               ; a board in virtual slot 20 before the final checksum
        mov di, [es:0]
        sub di, 2
        mov si, board
        mov cx, boardlen
        rep movsb
        add di, 2
        mov [es:0], di
        inc byte [es:8]
        mov cx, di
        sub cx, 2
        xor si, si
        xor ax, ax
        xor bx, bx
cksum:  mov bl, [es:si]
        add ax, bx
        inc si
        loop cksum
        neg ax
        mov [es:si], ax
        push word [biosds]              ; 43h: write it
        push word 0
        push es
        push word 0
        push word 43h
        call far [entry]
        add sp, 10
        or ax, ax
        jnz bad
        mov ax, [copyseg]               ; read back and compare
        call readescd
        push ds
        mov cx, [es:0]
        mov ds, [copyseg]
        xor si, si
        xor di, di
        repe cmpsb
        pop ds
        jne bad
        mov dx, s_written
        call puts

pmtest: mov eax, [nvbase]               ; ESCD selector at the NV storage base
        mov bx, gdt + 28h
        call setbase
        xor eax, eax
        mov ax, cs
        mov [rmjump + 2], ax
        shl eax, 4
        mov bx, gdt + 8                 ; this program's code and data
        call setbase
        mov bx, gdt + 10h
        call setbase
        add eax, gdt
        mov [gdtr + 2], eax
        cli
        movzx esp, sp
        lgdt [gdtr]
        mov eax, cr0
        or al, 1
        mov cr0, eax
        jmp 08h:pm
pm:     mov ax, 10h
        mov ds, ax
        mov es, ax
        mov ss, ax
        push word 20h                   ; 00h
        push word 10h
        push word pmmax
        push word 10h
        push word pmnodes
        push word 0
        call far [pmentry]
        add sp, 12
        mov [pm00], ax
        mov byte [handle], 0            ; 01h on every node
pmnode: push word 20h
        push word 1
        push word 10h
        push word nodebuf
        push word 10h
        push word handle
        push word 1
        call far [pmentry]
        add sp, 14
        or ax, ax
        jnz pmfail
        inc byte [pmcount]
        cmp byte [handle], 0ffh
        jne pmnode
pmfail: mov [pm01], ax
        push word 20h                   ; 42h through the ESCD selector
        push word 28h
        push word 10h
        push word pmescd
        push word 42h
        call far [pmentry]
        add sp, 10
        mov [pm42], ax
        push word 20h                   ; 43h: not in protected mode
        push word 28h
        push word 10h
        push word pmescd
        push word 43h
        call far [pmentry]
        add sp, 10
        mov [pm43], ax
        mov eax, cr0
        and al, 0feh
        mov cr0, eax
        jmp far [rmjump]
rm:     mov ax, cs
        mov ds, ax
        mov es, ax
        mov ss, ax
        sti
        mov dx, s_pm
        call puts
        mov al, [pmnodes]
        call hex8
        mov al, [pmcount]
        call hex8
        mov ax, [pm00]
        call hex16
        mov ax, [pm01]
        call hex16
        mov ax, [pm42]
        call hex16
        mov ax, [pm43]
        call hex16
        call crlf
        mov al, [pmnodes]               ; as in real mode, and the same ESCD
        cmp al, [nodes]
        jne pmbad
        cmp al, [pmcount]
        jne pmbad
        cmp word [pm42], 0
        jne pmbad
        cmp word [pm43], 82h
        jne pmbad
        mov es, [escdseg]
        mov cx, [es:0]
        mov si, pmescd
        xor di, di
        repe cmpsb
        jne pmbad
        mov dx, s_pmok
        call puts
        jmp done
pmbad:  mov dx, s_pmbad
        call puts
done:   mov dx, s_done
        call puts
        mov ax, 4c00h
        int 21h

bad:    push ax                         ; failed call: its code
        mov dx, s_fail
        call puts
        pop ax
        call hex16
        call crlf
        mov dx, s_done
fail:   call puts
        mov ax, 4c01h
        int 21h

setbase:                                ; base EAX into the descriptor at DS:BX
        push eax
        mov [bx + 2], ax
        shr eax, 16
        mov [bx + 4], al
        mov [bx + 7], ah
        pop eax
        ret

readescd:                               ; 42h into segment AX
        push word [biosds]
        push word 0
        push ax
        push word 0
        push word 42h
        call far [entry]
        add sp, 10
        or ax, ax
        jnz bad
        ret

eisa:   mov al, ' '                     ; compressed EISA ID at DS:SI
        call putc
        mov al, [si]
        shr al, 2
        call letter
        mov ax, [si]
        xchg al, ah
        shr ax, 5
        call letter
        mov al, [si + 1]
        call letter
        mov ah, [si + 2]
        mov al, [si + 3]
        jmp hex16b

letter: and al, 1fh
        add al, '@'
        jmp putc

hex16:  push ax
        mov al, ' '
        call putc
        pop ax
hex16b: push ax
        mov al, ah
        call hex8b
        pop ax
        jmp hex8b

hex8:   push ax
        mov al, ' '
        call putc
        pop ax
hex8b:  push ax
        shr al, 4
        call nibble
        pop ax
nibble: and al, 0fh
        add al, '0'
        cmp al, '9'
        jbe putc
        add al, 7
putc:   push dx
        push ax
        mov dl, al
        mov ah, 2
        int 21h
        pop ax
        pop dx
        ret

crlf:   mov dx, s_crlf
puts:   push ax
        mov ah, 9
        int 21h
        pop ax
        ret

board:  dw 24                           ; slot 20, ID PNP1234, one function: I/O 300h-307h
        db 20, 0, 41h, 0d0h, 12h, 34h, 40h, 40h, 0, 0
        dw 6
        db 1, 0, 10h, 07h
        dw 300h
        dw 0
        dw -(24 + 20 + 41h + 0d0h + 12h + 34h + 40h + 40h + 6 + 1 + 10h + 07h + 3)
boardlen equ $ - board

nopnp:     db "PNP-NO-HEADER", 13, 10, '$'
s_nodes:   db "PNP NODES$"
s_nodesok: db "PNP-NODES-OK", 13, 10, '$'
s_isa:     db "PNP ISA$"
s_escd:    db "ESCD$"
s_written: db "ESCD-WRITE-OK", 13, 10, '$'
s_fail:    db "PNP-CALL-FAIL$"
s_done:    db "PNP-TEST-DONE", 13, 10, '$'
s_crlf:    db 13, 10, '$'
s_pm:      db "PNP PM$"
s_pmok:    db "PNP-PM-OK", 13, 10, '$'
s_pmbad:   db "PNP-PM-FAIL", 13, 10, '$'

gdt:    dq 0                            ; 08h this code, 10h this data and stack, 18h BIOS code,
        dw 0ffffh, 0, 9a00h, 0          ; 20h BIOS data, 28h ESCD (NV storage): 16-bit, 64 KiB
        dw 0ffffh, 0, 9200h, 0
        dw 0ffffh, 0, 9a00h, 0
        dw 0ffffh, 0, 9200h, 0
        dw 0ffffh, 0, 9200h, 0
gdtr:   dw $ - gdt - 1
        dd 0
pmentry: dw 0, 18h
rmjump:  dw rm, 0
pmnodes: db 0
pmcount: db 0
pmmax:   dw 0
pm00:    dw 0
pm01:    dw 0
pm42:    dw 0
pm43:    dw 0
pmescd  equ 9000h

entry:    dd 0
biosds:   dw 0
escdseg:  dw 0
copyseg:  dw 0
nodes:    db 0
count:    db 0
handle:   db 0
nodemax:  dw 0
isacfg:   times 6 db 0
minsize:  dw 0
escdsize: dw 0
nvbase:   dd 0
nodebuf:
