global longmode_interrupt_handlers
global ap_init_patch_idt_lm
global idt_table_lm
global idt_descriptor_lm
%include "checkpoint.inc"
%define LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC  0x13
%define LONG_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC  0x14
extern longmode_enter_checkpoint
; IDT定义 - 长模式64位下的前32个向量
SECTION .ap_bootstrap_text
; 长模式下的中断处理例程标签
longmode_interrupt_handlers:
bits 64
    .divide_by_zero:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 0
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .debug:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 1
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .nmi:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 2
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .bp:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 3
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .of:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 4
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .br:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 5
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .ud:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 6
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .nm:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 7
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .df:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 8
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .cross:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 9
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .tss:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 10
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .NP:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 11
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .SS:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 12
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .GP:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 13
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .PF:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 14
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .vec15:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 15
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .MF:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 16
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .AC:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 17
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .MC:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 18
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .XM:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 19
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .VE:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_NO_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 20
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt
    
    .CP:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, cr0
    push rax
    mov rax, cr2
    push rax
    mov rax, cr3
    push rax
    mov rax, cr4
    push rax
    xor rax, rax
    mov ax, es
    push rax
    mov ax, ds
    push rax
    mov ax, ss
    push rax
    mov ax, fs
    push rax
    mov ax, gs
    push rax
    mov ecx, 0xC0000080
    rdmsr
    push rax
    mov rax, LONG_FINAL_STACK_WITH_ERRCODE_TOP_MAGIC
    push rax
    
    ; 使用通用寄存器 + 64位绝对寻址
    mov rbx, qword longmode_enter_checkpoint  ; 加载64位绝对地址到寄存器
    mov word [rbx + check_point.failure_final_stack_top], sp
    mov byte [rbx + check_point.failure_caused_excption_num], 21
    sfence
    mov byte [rbx + check_point.failure_flags], 0x3
    hlt

ap_init_patch_idt_lm:
    push rax
    push rbx
    mov rbx, qword idt_table_lm

    mov rax, qword longmode_interrupt_handlers.divide_by_zero
    mov word  [rbx + 0*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 0*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 0*16 + 8], eax
    mov dword [rbx + 0*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.debug
    mov word  [rbx + 1*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 1*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 1*16 + 8], eax
    mov dword [rbx + 1*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.nmi
    mov word  [rbx + 2*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 2*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 2*16 + 8], eax
    mov dword [rbx + 2*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.bp
    mov word  [rbx + 3*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 3*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 3*16 + 8], eax
    mov dword [rbx + 3*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.of
    mov word  [rbx + 4*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 4*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 4*16 + 8], eax
    mov dword [rbx + 4*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.br
    mov word  [rbx + 5*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 5*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 5*16 + 8], eax
    mov dword [rbx + 5*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.ud
    mov word  [rbx + 6*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 6*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 6*16 + 8], eax
    mov dword [rbx + 6*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.nm
    mov word  [rbx + 7*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 7*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 7*16 + 8], eax
    mov dword [rbx + 7*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.df
    mov word  [rbx + 8*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 8*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 8*16 + 8], eax
    mov dword [rbx + 8*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.cross
    mov word  [rbx + 9*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 9*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 9*16 + 8], eax
    mov dword [rbx + 9*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.tss
    mov word  [rbx + 10*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 10*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 10*16 + 8], eax
    mov dword [rbx + 10*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.NP
    mov word  [rbx + 11*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 11*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 11*16 + 8], eax
    mov dword [rbx + 11*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.SS
    mov word  [rbx + 12*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 12*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 12*16 + 8], eax
    mov dword [rbx + 12*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.GP
    mov word  [rbx + 13*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 13*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 13*16 + 8], eax
    mov dword [rbx + 13*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.PF
    mov word  [rbx + 14*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 14*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 14*16 + 8], eax
    mov dword [rbx + 14*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.vec15
    mov word  [rbx + 15*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 15*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 15*16 + 8], eax
    mov dword [rbx + 15*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.MF
    mov word  [rbx + 16*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 16*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 16*16 + 8], eax
    mov dword [rbx + 16*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.AC
    mov word  [rbx + 17*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 17*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 17*16 + 8], eax
    mov dword [rbx + 17*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.MC
    mov word  [rbx + 18*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 18*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 18*16 + 8], eax
    mov dword [rbx + 18*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.XM
    mov word  [rbx + 19*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 19*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 19*16 + 8], eax
    mov dword [rbx + 19*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.VE
    mov word  [rbx + 20*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 20*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 20*16 + 8], eax
    mov dword [rbx + 20*16 + 12], 0

    mov rax, qword longmode_interrupt_handlers.CP
    mov word  [rbx + 21*16 + 0], ax
    shr rax, 16
    mov word  [rbx + 21*16 + 6], ax
    shr rax, 16
    mov dword [rbx + 21*16 + 8], eax
    mov dword [rbx + 21*16 + 12], 0

    pop rbx
    pop rax
    ret
SECTION .ap_bootstrap_data
idt_table_lm:
    ; IDT条目格式 (每个条目16字节):
    ; 偏移[0:1] - 偏移地址的低16位
    ; 段选择子[2:3] - 代码段选择子
    ; IST[7:3] - 中断栈表索引
    ; 类型和属性[4] - 保留
    ; 类型和属性[5] - 门类型描述符
    ; IST[7:4] - 中断栈表高4位
    ; 偏移[6:7] - 偏移地址的中间16位
    ; 偏移[8:11] - 偏移地址的高32位
    ; 保留[12:15] - 必须为0
    dw 0                                             ; Vector 0 - Divide by Zero (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 1 - Debug (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 2 - NMI (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 3 - Breakpoint (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 4 - Overflow (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 5 - Bound Range (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 6 - Invalid Opcode (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 7 - Device Not Available (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 8 - Double Fault (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 9 - Coprocessor Segment Overrun (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 10 - Invalid TSS (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 11 - Segment Not Present (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 12 - Stack Segment Fault (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 13 - General Protection (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 14 - Page Fault (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 15 - Reserved (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 16 - x87 FPU Floating Point Error (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 17 - Alignment Check (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 18 - Machine Check (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 19 - SIMD Floating Point Exception (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 20 - Virtualization Exception (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved
    
    dw 0                                             ; Vector 21 - Control Protection Exception (offset low)
    dw 0x18                                          ; Code segment selector
    db 0                                             ; IST and reserved
    db 0x8E                                          ; Type and attributes (Interrupt Gate)
    dw 0                                             ; High part of offset (bits 31:16)
    dd 0                                             ; Highest part of offset (bits 63:32)
    dd 0                                             ; Reserved

; 长模式IDT描述符
idt_descriptor_lm:
    dw $ - idt_table_lm - 1     ; Limit (size of IDT - 1)
    dq idt_table_lm             ; Base address of IDT (64-bit)
