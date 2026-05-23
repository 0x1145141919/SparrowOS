global idt_descriptor_pe
global idt_table_pe
global pe_interrupt_vector
global ap_init_patch_idt_pe
%define PE_FINAL_STACK_NO_ERRCODE_TOP_MAGIC   0x11
%define PE_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC 0x12
%include "checkpoint.inc"
extern pemode_enter_checkpoint

; ── 宏定义 ──

; %1: 标签名  %2: 异常编号
%macro pe_handler_no_err 2
    .%1:
    pushad
    push es
    push ds
    push ss
    push fs
    push gs
    mov eax, cr0
    push eax
    mov eax, cr2
    push eax
    mov eax, cr3
    push eax
    mov eax, cr4
    push eax
    mov ecx, 0xC0000080
    rdmsr
    push eax
    mov eax, PE_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push eax
    mov byte [pemode_enter_checkpoint+check_point.failure_caused_excption_num], %2
    mov word [pemode_enter_checkpoint+check_point.failure_final_stack_top], sp
    sfence
    mov byte [pemode_enter_checkpoint+check_point.failure_flags], 0x3
    hlt
%endmacro

; %1: 标签名  %2: 异常编号
%macro pe_handler_with_err 2
    .%1:
    pushad
    push es
    push ds
    push ss
    push fs
    push gs
    mov eax, %2
    push eax
    mov eax, cr0
    push eax
    mov eax, cr2
    push eax
    mov eax, cr3
    push eax
    mov eax, cr4
    push eax
    mov ecx, 0xC0000080
    rdmsr
    push eax
    mov eax, PE_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC
    push eax
    mov byte [pemode_enter_checkpoint+check_point.failure_caused_excption_num], %2
    mov word [pemode_enter_checkpoint+check_point.failure_final_stack_top], sp
    sfence
    mov byte [pemode_enter_checkpoint+check_point.failure_flags], 0x3
    hlt
%endmacro

; %1: 标签名  %2: 异常编号
%macro patch_pe_idt_entry 2
    mov rax, qword pe_interrupt_handlers.%1
    mov word  [rbx + %2*8 + 0], ax
    shr rax, 16
    mov word  [rbx + %2*8 + 6], ax
%endmacro

; ── 数据段 ──

SECTION .ap_bootstrap_data
idt_table_pe:
    times 22 dw 0, 0x10, 0x8E00, 0
idt_descriptor_pe:
    dw $ - idt_table_pe - 1
    dd idt_table_pe

; ── 代码段 ──

SECTION .ap_bootstrap_text
pe_interrupt_handlers:
bits 32
    ; 第 0 类：无错误码
    pe_handler_no_err divide_by_zero, 0
    pe_handler_no_err debug,          1
    pe_handler_no_err nmi,            2
    pe_handler_no_err bp,             3
    pe_handler_no_err of,             4
    pe_handler_no_err br,             5
    pe_handler_no_err ud,             6
    pe_handler_no_err nm,             7
    ; 第 0 类结束

    ; 第 1 类：有错误码
    pe_handler_with_err df,            8
    pe_handler_no_err  cross,          9   ; 无错误码
    pe_handler_with_err tss,          10
    pe_handler_with_err NP,           11
    pe_handler_with_err SS,           12
    pe_handler_with_err GP,           13
    pe_handler_with_err PF,           14
    ; 第 1 类结束

    ; 第 2 类：无错误码
    pe_handler_no_err vec15,         15
    pe_handler_no_err MF,            16

    pe_handler_with_err AC,           17  ; AC 有错误码
    pe_handler_no_err  MC,            18
    pe_handler_no_err  XM,            19
    pe_handler_no_err  VE,            20
    pe_handler_with_err CP,           21  ; CP 有错误码
SECTION .text
bits 64
ap_init_patch_idt_pe:
    push rax
    push rbx
    mov rbx, qword idt_table_pe

    patch_pe_idt_entry divide_by_zero,  0
    patch_pe_idt_entry debug,           1
    patch_pe_idt_entry nmi,             2
    patch_pe_idt_entry bp,              3
    patch_pe_idt_entry of,              4
    patch_pe_idt_entry br,              5
    patch_pe_idt_entry ud,              6
    patch_pe_idt_entry nm,              7
    patch_pe_idt_entry df,              8
    patch_pe_idt_entry cross,           9
    patch_pe_idt_entry tss,            10
    patch_pe_idt_entry NP,             11
    patch_pe_idt_entry SS,             12
    patch_pe_idt_entry GP,             13
    patch_pe_idt_entry PF,             14
    patch_pe_idt_entry vec15,          15
    patch_pe_idt_entry MF,             16
    patch_pe_idt_entry AC,             17
    patch_pe_idt_entry MC,             18
    patch_pe_idt_entry XM,             19
    patch_pe_idt_entry VE,             20
    patch_pe_idt_entry CP,             21

    pop rbx
    pop rax
    ret
