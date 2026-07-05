bits 64
%pragma limit rep 2000000
global bsp_init_gdt_descriptor
global bsp_init_idtr
global bsp_init_idt_entries
extern __init_stack_end
extern init
extern div_by_zero_bare_enter
extern breakpoint_bare_enter
extern nmi_bare_enter
extern overflow_bare_enter
extern invalid_opcode_bare_enter
extern general_protection_bare_enter
extern double_fault_bare_enter
extern page_fault_bare_enter
extern machine_check_bare_enter
extern invalid_tss_bare_enter
extern simd_floating_point_bare_enter
extern virtualization_bare_enter
extern wrmsr_func
global _init_entry
global shift_kernel
global _low_4gb_pte_arr
global pdpte_arr
section .data
align 4096
_low_4gb_pte_arr:
    %assign i 0
    %rep 0x100000
        dq (i<<12) + 3
        %assign i i+1
    %endrep
_low_4gb_pd_arr:
    %assign i 0
    %rep 0x800
        dq _low_4gb_pte_arr + (i<<12) + 3
        %assign i i+1
    %endrep
pdpte_arr:
    %assign i 0
    %rep 0x4
        dq _low_4gb_pd_arr + (i<<12) + 3
        %assign i i+1
    %endrep
    %assign i 4
    %rep 0x1000 - 4
        dq (i<<30) + (1<<7) +3
        %assign i i+1
    %endrep
root_table:
    dq pdpte_arr +3
    dq pdpte_arr +3 + (1<<12)
    dq pdpte_arr +3 + (2<<12)
    dq pdpte_arr +3 + (3<<12)
    dq pdpte_arr +3 + (4<<12)
    dq pdpte_arr +3 + (5<<12)
    dq pdpte_arr +3 + (6<<12)
    dq pdpte_arr +3 + (7<<12)
    %assign i 0
    %rep 0x200 - 8
        dq 0
        %assign i i+1
    %endrep

; ── 初始化阶段 GDT（过渡用；phase 4.5 C 代码加载 proper GDT+TSS）──
; 索引 0: Null
; 索引 1 (K_cs_idx): 64-bit 内核代码段, selector 0x8
;    access = p(1), dpl(00), s(1), type(1001=execute_only) = 10011001b = 0x99
;    flags = limit1(1111), avl(0), l(1), d(0), g(1) = 10101111b = 0xAF
; 索引 2 (K_ds_ss_idx): 64-bit 内核数据段, selector 0x10
;    access = p(1), dpl(00), s(1), type(0011=read_write) = 10010011b = 0x93
;    flags = limit1(1111), avl(0), l(0), d(1), g(1) = 11001111b = 0xCF
bsp_init_gdt:
    dq 0                            ; Null
    dw 0xFFFF, 0x0000, 0x00, 0x99, 0xAF, 0x00    ; K_cs 64-bit
    dw 0xFFFF, 0x0000, 0x00, 0x93, 0xCF, 0x00    ; K_ds_ss
bsp_init_gdt_end:

; ── GDT 描述符（10 字节：2B limit + 8B base，64-bit LGDT 格式）──
align 8
bsp_init_gdt_descriptor:
    dw bsp_init_gdt_end - bsp_init_gdt - 1
    dq bsp_init_gdt

; ── 初始化阶段 IDT 条目数组（256 项 × 16 字节 = 4096 字节）──
; SET_IDT_OFFSET 宏在启动时填入特定向量
align 4096
bsp_init_idt_entries:
    times 256 dq 0, 0

; ── IDTR（10 字节：2B limit + 8B base，64-bit LIDT 格式）──
align 8
bsp_init_idtr:
    dw (256 * 16) - 1       ; limit = 4095
    dq bsp_init_idt_entries ; base

section .text

%macro SET_IDT_OFFSET 2
    mov rax, %2
    mov word [rbx + %1*16 + 0], ax
    shr rax, 16
    mov word [rbx + %1*16 + 6], ax
    shr rax, 16
    mov dword [rbx + %1*16 + 8], eax
    mov dword [rbx + %1*16 + 12], 0
%endmacro

; ── init_jump_to_kernel ───────────────────────────────────────────────
; void init_jump_to_kernel(x64_standard_context* ctx);
;
; 从 Phase 4.5 跳入 kernel.elf，等同于 idt_style_load 的语义。
; RDI 指向 x64_standard_context（含 15 个 GPR + iret_complex）。
; 该函数 pop 所有 GPR 后 iretq，永不返回。
global init_jump_to_kernel
init_jump_to_kernel:
    mov rsp, rdi
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    pop rbp
    iretq

shift_kernel:
    ; rdi = info_pbase (恒等映射内，可直接解引用)
    ; rsi = stack_bottom (kernel.elf 的 BSP 栈底)
    ; rdx = entry_vaddr (kernel.elf 入口高半虚拟地址)
    ; CR3 已由 Phase 4.5 C 代码切换到 KMMU 页表
    mov rsp, rsi
    call rdx

_init_entry:
    mov rax, root_table
    mov cr3, rax
    lea rax, [rel bsp_init_gdt_descriptor]
    lgdt [rax]
    mov r15, rdi
    mov rax, wrmsr_func
    mov rdi, 0x277
    mov rsi, 0x0407050600070106
    call rax
    
    lea rbx, [rel bsp_init_idt_entries]
    SET_IDT_OFFSET 0, div_by_zero_bare_enter
    SET_IDT_OFFSET 2, nmi_bare_enter
    SET_IDT_OFFSET 3, breakpoint_bare_enter
    SET_IDT_OFFSET 4, overflow_bare_enter
    SET_IDT_OFFSET 6, invalid_opcode_bare_enter
    SET_IDT_OFFSET 8, double_fault_bare_enter
    SET_IDT_OFFSET 10, invalid_tss_bare_enter
    SET_IDT_OFFSET 13, general_protection_bare_enter
    SET_IDT_OFFSET 14, page_fault_bare_enter
    SET_IDT_OFFSET 18, machine_check_bare_enter
    SET_IDT_OFFSET 19, simd_floating_point_bare_enter
    SET_IDT_OFFSET 20, virtualization_bare_enter

    lea rax, [rel bsp_init_idtr]
    lidt [rax]
    mov rdi, r15
    lea rsp, [rel __init_stack_end]
    call init

init_hang:
    hlt
    jmp init_hang
