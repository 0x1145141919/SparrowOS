global realmode_interrupt_handlers
global idt_descriptor_rm
global idt_table_rm
global ap_init_patch_idt_rm
%include "checkpoint.inc"
%define realmode_magic 0x15
extern realmode_enter_checkpoint
SECTION .ap_bootstrap_text
realmode_interrupt_handlers:
bits 16
    .divide_by_zero:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax ; 只保留低16位，尤其是PE位
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 0
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .debug:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 1
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .nmi:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 2
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .bp:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 3
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .of:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 4
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .br:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 5
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .ud:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 6
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .nm:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 7
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .df:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 8
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .cross:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 9
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .tss:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 10
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .NP:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 11
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .SS:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 12
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .GP:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 13
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .PF:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 14
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .vec15:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 15
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .MF:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 16
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .AC:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 17
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .MC:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 18
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .XM:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 19
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .VE:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 20
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt
    
    .CP:
    pusha
    push es
    push ds
    push ss
    push fs
    push gs
    mov cr0, eax
    push ax
    push realmode_magic
    mov word [realmode_enter_checkpoint+check_point.failure_final_stack_top], sp
    mov byte [realmode_enter_checkpoint+check_point.failure_caused_excption_num], 21
    sfence
    mov byte [realmode_enter_checkpoint+check_point.failure_flags], 3
    hlt

ap_init_patch_idt_rm:
bits 64
    push rax
    push rbx
    mov rbx, qword idt_table_rm

    mov rax, qword realmode_interrupt_handlers.divide_by_zero
    mov word [rbx + 0*4 + 0], ax
    mov word [rbx + 0*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.debug
    mov word [rbx + 1*4 + 0], ax
    mov word [rbx + 1*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.nmi
    mov word [rbx + 2*4 + 0], ax
    mov word [rbx + 2*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.bp
    mov word [rbx + 3*4 + 0], ax
    mov word [rbx + 3*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.of
    mov word [rbx + 4*4 + 0], ax
    mov word [rbx + 4*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.br
    mov word [rbx + 5*4 + 0], ax
    mov word [rbx + 5*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.ud
    mov word [rbx + 6*4 + 0], ax
    mov word [rbx + 6*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.nm
    mov word [rbx + 7*4 + 0], ax
    mov word [rbx + 7*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.df
    mov word [rbx + 8*4 + 0], ax
    mov word [rbx + 8*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.cross
    mov word [rbx + 9*4 + 0], ax
    mov word [rbx + 9*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.tss
    mov word [rbx + 10*4 + 0], ax
    mov word [rbx + 10*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.NP
    mov word [rbx + 11*4 + 0], ax
    mov word [rbx + 11*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.SS
    mov word [rbx + 12*4 + 0], ax
    mov word [rbx + 12*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.GP
    mov word [rbx + 13*4 + 0], ax
    mov word [rbx + 13*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.PF
    mov word [rbx + 14*4 + 0], ax
    mov word [rbx + 14*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.vec15
    mov word [rbx + 15*4 + 0], ax
    mov word [rbx + 15*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.MF
    mov word [rbx + 16*4 + 0], ax
    mov word [rbx + 16*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.AC
    mov word [rbx + 17*4 + 0], ax
    mov word [rbx + 17*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.MC
    mov word [rbx + 18*4 + 0], ax
    mov word [rbx + 18*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.XM
    mov word [rbx + 19*4 + 0], ax
    mov word [rbx + 19*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.VE
    mov word [rbx + 20*4 + 0], ax
    mov word [rbx + 20*4 + 2], 0

    mov rax, qword realmode_interrupt_handlers.CP
    mov word [rbx + 21*4 + 0], ax
    mov word [rbx + 21*4 + 2], 0

    mov word [idt_descriptor_rm], idt_table_rm_end - idt_table_rm - 1
    mov rax, qword idt_table_rm
    mov dword [idt_descriptor_rm + 2], eax
    pop rbx
    pop rax
    ret

SECTION .ap_bootstrap_data
; IDT定义 - 实模式16位下的前32个向量（实际上实模式使用向量跳转表，此处为模拟结构）
idt_table_rm:
    ; 实模式下实际不使用IDT，而是使用向量跳转表
    ; 但为了保持一致性，这里仍定义类似结构
    ; 每个条目4字节: 偏移量(2字节) + 段地址(2字节)
    times 44 dw 0
idt_table_rm_end:

; 实模式IDT描述符（实际上实模式使用中断向量表IVT，位于内存地址0x0000:0x0000-0x0000:0x03FF）
idt_descriptor_rm:
    dw $ - idt_table_rm - 1     ; Limit (size of IDT - 1)
    dd idt_table_rm             ; Base address of IDT (simulated for real mode)
