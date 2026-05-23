ENTRY(_kernel_Init)
__KImgSize = 128M;
EXTERN(PAGE_LEVEL)
PAGE_LEVEL_5 = 0xFF00000000000000;
PAGE_LEVEL_4 = 0xFFFF800000000000;
ap_bootstrap_base = 0x4000;
init_sec_size   = 0x100000;
KImgphybase = 0x2000000;
PROVIDE(KImgphybase = 0x2000000);


KImgvbase = PAGE_LEVEL_4;
Koffset = KImgvbase - KImgphybase;
base_kernel_address = KImgvbase;

MEMORY {
    /* 虚拟内存内核主体区域 */
    VRAM (rwx) : ORIGIN = KImgvbase, LENGTH = __KImgSize
    /*物理内存初始化代码区 */
    init (rx) : ORIGIN = ap_bootstrap_base, LENGTH = init_sec_size
}

SECTIONS { 
    . = ap_bootstrap_base;
    .ap_bootstrap_text : {
        ap_bootstrap_begin = .;
        *(.ap_bootstrap_text)
        . = ALIGN(4096);
        ap_bootstrap_end = .;
    } > init : ap_bootstrap_text
    .ap_bootstrap_rodata : {
        ap_bootstrap_begin = .;
        *(.ap_bootstrap_rodata)
        . = ALIGN(4K);
        ap_bootstrap_end = .;
    } > init : ap_bootstrap_rodata
    .ap_bootstrap_data : {
        ap_bootstrap_begin = .;
        *(.ap_bootstrap_data)
        . = ALIGN(4K);
        ap_bootstrap_end = .;
    } > init : ap_bootstrap_data
    .ap_bootstrap_stack : {
        ap_bootstrap_begin = .;
        *(.ap_bootstrap_stack)
        . = ALIGN(4K);
        ap_bootstrap_end = .;
    } > init : ap_bootstrap_stack
    . = KImgvbase;
    .text_main : {
        text_begin = .;
         *(.text .text.*)
         . = ALIGN(2M);
         text_end = . ;
    } > VRAM :text_main
    .data : {
        _data_start = .;
        *(.data .data.*)
         *(.got .got.*)
         *(.*array)
        *(COMMON)
        *(.gnu.property)
    . = ALIGN(4096); 
    _data_end = . ;
    } > VRAM :data
    .rodata : {
        . = ALIGN(2M);
        _rodata_start = . ;
         *(.rodata .rodata.*)
         *(.eh_frame)
    . = ALIGN(2M);
    _rodata_end = . ;
    } > VRAM :rodata
    . = ALIGN(4096);
    
 
    .bss :  {
        . = ALIGN(2M);
        _bss_begin = .;
        *(.bss .bss.*)
        *(.lbss .lbss.*)
        _bss_end = .;
        . = ALIGN(2M);
    } > VRAM :bss

    /* NOTE 段（PT_NOTE，非 PT_LOAD；但仍可经文件镜像访问）*/
    .note : {
        *(.note.gnu.build-id)
        *(.note.*)
    } > VRAM :notes
}
/* 定义段权限 */
PHDRS {
    ap_bootstrap_text PT_LOAD FLAGS(5);
    ap_bootstrap_rodata PT_LOAD FLAGS(4);
    ap_bootstrap_data PT_LOAD FLAGS(6);
    ap_bootstrap_stack PT_LOAD FLAGS(6);
    text_main PT_LOAD FLAGS(5 | 0x100);      /* PF_R | PF_X (RX) */
    data PT_LOAD FLAGS(6 | 0x100);      /* PF_R | PF_W (RW) */   
    rodata PT_LOAD FLAGS(4 | 0x100);    /* PF_R (R) */
    bss PT_LOAD FLAGS(6 | 0x100);
    notes PT_NOTE;
}