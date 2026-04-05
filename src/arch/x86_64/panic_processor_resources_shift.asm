section .text
global resources_shift
extern bsp_init_idtr
extern bsp_init_gdt_descriptor
extern pml4_table_init
extern pml5_table_init
%define K_cs_idx 8
resources_shift:
bits 64
    sub rsp, 8
    ;mov rax, pml4_table_init
    ;mov cr3, rax
    mov rax, bsp_init_gdt_descriptor
    lgdt [rax]
    mov rax, K_cs_idx
    push rax
    mov rax, .load_cs_finish
    push rax
    retfq
.load_cs_finish:
    mov rax, bsp_init_idtr
    lidt [rax]
    mov rax, 16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    add rsp, 8
    ret
    