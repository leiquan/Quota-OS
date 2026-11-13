/*
 * Quota OS 链接脚本。
 * QEMU virt 把 -kernel 映像放在 RAM 起点之上 0x80000 处，
 * 即物理地址 0x40080000。入口必须落在这里。
 */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x40080000;
    __image_start = .;

    .text.boot : {
        KEEP(*(.text.boot))
    } :text

    .text : {
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :text

    .data : {
        *(.data .data.*)
    } :data

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data
    __image_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
    }
}
