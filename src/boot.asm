[BITS 16]
[ORG 0x7C00]

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 512
%endif

TABLE_ADDR equ 0x8000
FDC_SPT    equ 18
FDC_HEADS  equ 2
BOOT_SEG   equ 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    call clear_table
    call do_e820
    call calc_mem_size
    call get_hdd_size

    mov word [cur_lba], 1
    mov word [buf_seg], BOOT_SEG
    mov word [buf_off], 0
    mov word [loop_cnt], KERNEL_SECTORS

read_loop:
    mov ax, [cur_lba]
    call lba_to_chs

    mov ax, [buf_off]
    mov bx, ax
    mov ax, [buf_seg]
    mov es, ax

    mov al, 1
    mov ah, 0x02
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    inc word [cur_lba]

    add word [buf_off], 512
    jnc .no_wrap
    mov word [buf_off], 0
    mov ax, [buf_seg]
    add ax, 0x1000
    mov [buf_seg], ax
.no_wrap:
    dec word [loop_cnt]
    jnz read_loop

    cli

    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

    lgdt [gdt_desc]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:protected_mode

lba_to_chs:
    xor dx, dx
    mov cx, FDC_SPT * FDC_HEADS
    div cx
    mov bx, ax
    mov ax, dx
    xor dx, dx
    mov cx, FDC_SPT
    div cx
    mov dh, al
    mov cl, dl
    inc cl
    mov ch, bl
    ret

clear_table:
    mov di, TABLE_ADDR
    mov cx, 0x1000 / 2
    xor ax, ax
    rep stosw
    ret

do_e820:
    mov di, TABLE_ADDR + 12
    xor bp, bp
    xor ebx, ebx

.e820_loop:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done

    inc bp
    add di, 24
    cmp bp, 128
    jae .e820_done

    test ebx, ebx
    jnz .e820_loop

.e820_done:
    mov [TABLE_ADDR], bp
    ret

calc_mem_size:
    mov dword [TABLE_ADDR + 4], 0
    mov si, TABLE_ADDR + 12
    mov cx, [TABLE_ADDR]
    test cx, cx
    jz .mem_done

.mem_loop:
    cmp dword [si + 16], 1
    jne .mem_next

    mov eax, [si + 8]
    add [TABLE_ADDR + 4], eax

.mem_next:
    add si, 24
    loop .mem_loop

.mem_done:
    ret

get_hdd_size:
    mov dword [TABLE_ADDR + 8], 0

    mov ah, 0x48
    mov dl, 0x80
    mov si, disk_param
    int 0x13
    jc .disk_done

    mov eax, [disk_param + 16]
    mov [TABLE_ADDR + 8], eax

.disk_done:
    ret

[BITS 32]
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    mov word [0xB8000], 0x074D

    mov eax, cr0
    and eax, ~(1 << 2)
    or  eax, (1 << 1)
    mov cr0, eax

    mov eax, cr4
    or  eax, (1 << 9) | (1 << 10)
    mov cr4, eax

    mov ebx, TABLE_ADDR
    jmp 0x10000

disk_error:
    mov si, msg
.print:
    lodsb
    or al, al
    jz .hang
    mov ah, 0x0E
    int 0x10
    jmp .print
.hang:
    hlt
    jmp .hang

boot_drive db 0
msg db "disk error", 0
loop_cnt dw 0

align 4
cur_lba:  dw 0, 0
buf_seg:  dw 0
buf_off:  dw 0

align 4
disk_param:
    dw 0x001A
    dw 0
    dd 0
    dd 0
    dd 0
    dd 0
    dd 0
    dd 0
    dd 0

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:

gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

times 510 - ($ - $$) db 0
dw 0xAA55
