ENTRY(_start)

SECTIONS
{
    . = 0x10000;
    __kernel_start = .;

    .text : ALIGN(4096)
    {
        *(.text._start)
        *(.text)
        *(.text.*)
    }
    __kernel_text_end = .;

    .rodata : ALIGN(4096)
    {
        *(.rodata)
        *(.rodata.*)
    }
    __kernel_rodata_end = .;

    .data : ALIGN(4096)
    {
        *(.data)
        *(.data.*)
    }
    __kernel_data_end = .;

    __kernel_image_end = .;
    ASSERT(__kernel_image_end <= 0xA0000, "kernel image exceeds 0x9FFFF (VGA)")

    . = 0x1000000;
    .bss : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }
    __kernel_bss_end = .;
    ASSERT(__kernel_bss_end <= 0x1200000, "bss exceeds 16MB..18MB")

    . = 0x1200000;
    __kernel_heap_start = .;
    . = 0x3000000;
    __kernel_heap_end = .;

    __kernel_fs_start = .;
    . = 0x4000000;
    __kernel_fs_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
    }
}
