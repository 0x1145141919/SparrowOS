section .text

%macro INTERRUPT_ENTRY_WITH_ERRCODE 1

    push rbp
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, %1
    call rax

    pop rsp ;栈自动回落
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
%endmacro
extern iommu_fault_cpp_enter
global iommu_fault_deal
iommu_fault_deal:
INTERRUPT_ENTRY_WITH_ERRCODE iommu_fault_cpp_enter
extern i8042_cpp_enter
global i8042_fault_deal
i8042_fault_deal:
INTERRUPT_ENTRY_WITH_ERRCODE i8042_cpp_enter