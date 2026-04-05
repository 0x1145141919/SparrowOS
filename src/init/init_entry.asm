bits 64
%pragma limit rep 2000000
extern bsp_init_gdt_descriptor
extern bsp_init_idtr
extern bsp_init_idt_entries
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

shift_kernel:
    mov rax, [rdi+0x20]
    mov cr3, rax
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
