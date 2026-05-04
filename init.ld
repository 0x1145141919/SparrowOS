ENTRY(_init_entry)

INIT_BASE = 0x100000000;
INIT_IMAGE_SIZE = 16M;
INIT_STACK_SIZE = 64K;
base_kernel_address = INIT_BASE;
INIT_HEAP_SIZE = 2M;

PROVIDE(INIT_BASE = INIT_BASE);
PROVIDE(INIT_IMAGE_SIZE = INIT_IMAGE_SIZE);
PROVIDE(INIT_STACK_SIZE = INIT_STACK_SIZE);
PROVIDE(INIT_HEAP_SIZE = INIT_HEAP_SIZE);

MEMORY {
    INIT_RAM (rwx) : ORIGIN = INIT_BASE, LENGTH = INIT_IMAGE_SIZE
}

SECTIONS {
    . = ORIGIN(INIT_RAM);

    .text : ALIGN(4096) {
        __init_text_start = .;
        *(.text .text.*)
        __init_text_end = .;
    } > INIT_RAM :text

    .rodata : ALIGN(4096) {
        __init_rodata_start = .;
        *(.rodata .rodata.*)
        *(.eh_frame)
        __init_rodata_end = .;
    } > INIT_RAM :rodata

    .data : ALIGN(4096) {
        __init_data_start = .;
        *(.data .data.*)
        *(.got .got.*)
        *(.*array)
        *(.note.*)
        *(.gnu.property)
        __init_data_end = .;
    } > INIT_RAM :data

    .bss (NOLOAD) : ALIGN(4096) {
        __init_bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        *(.lbss .lbss.*)
        __init_bss_end = .;
    } > INIT_RAM :bss

    .stack (NOLOAD) : ALIGN(4096) {
        __init_stack_start = .;
        . = . + INIT_STACK_SIZE;
        __init_stack_end = .;
    } > INIT_RAM :stack

    .heap (NOLOAD) : ALIGN(4096) {
        __init_heap_start = .;
        . = . + INIT_HEAP_SIZE;
        __init_heap_end = .;
    } > INIT_RAM :heap

    PROVIDE(__init_text_size = SIZEOF(.text));
    PROVIDE(__init_rodata_size = SIZEOF(.rodata));
    PROVIDE(__init_data_size = SIZEOF(.data));
    PROVIDE(__init_bss_size = SIZEOF(.bss));
    PROVIDE(__init_stack_size = SIZEOF(.stack));
    PROVIDE(__init_heap_size = SIZEOF(.heap));

    PROVIDE(__init_image_start = ORIGIN(INIT_RAM));
    PROVIDE(__init_image_end = .);
    PROVIDE(__init_image_size = __init_image_end - __init_image_start);
}

PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    bss PT_LOAD FLAGS(6);
    stack PT_LOAD FLAGS(6);
    heap PT_LOAD FLAGS(6);
}
