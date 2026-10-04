[BITS 32]

section .text

global proc_jump

proc_jump:
    mov eax, [esp + 4]
    mov edx, [esp + 8]
    mov cr3, eax
    mov esp, edx

    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popad
    add esp, 8
    iret
