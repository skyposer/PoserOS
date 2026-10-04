[BITS 32]

section .text._start
global _start
extern kernel_main
extern __bss_start
extern __bss_end

_start:
    cli
    mov esp, 0x90000

    mov word [0xB8002], 0x074B

    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    mov word [0xB8004], 0x0742

    push ebx
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang