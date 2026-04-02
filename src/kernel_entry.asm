; 物理地址类型 (MAXPHYADDR ≤ 52)
%define phys_addr_t dq
%include "check_point.inc"
; 页大小数组
%define PAGE_SIZE_IN_LV_0 (1 << 12)    ; NORMAL_4kb_MASK_OFFSET
%define PAGE_SIZE_IN_LV_1 (1 << 21)    ; NORMAL_2MB_MASK_OFFSET
%define PAGE_SIZE_IN_LV_2 (1 << 30)    ; NORMAL_1GB_MASK_OFFSET
%define PAGE_SIZE_IN_LV_3 (1 << 39)    ; NORMAL_512GB_MASK_OFFSET
%define PAGE_SIZE_IN_LV_4 (1 << 48)    ; NORMAL_256TB_MASK_OFFSET

; 页偏移掩码数组
%define PAGE_OFFSET_MASK_0 ((1 << 12) - 1)   ; NORMAL_4kb_MASK_OFFSET
%define PAGE_OFFSET_MASK_1 ((1 << 21) - 1)   ; NORMAL_2MB_MASK_OFFSET
%define PAGE_OFFSET_MASK_2 ((1 << 30) - 1)   ; NORMAL_1GB_MASK_OFFSET
%define PAGE_OFFSET_MASK_3 ((1 << 39) - 1)   ; NORMAL_512GB_MASK_OFFSET
%define PAGE_OFFSET_MASK_4 ((1 << 48) - 1)   ; NORMAL_256TB_MASK_OFFSET

; 物理地址宽度（假设为52位）
%define PHYS_ADDR_WIDTH 52
%define PHYS_ADDR_MASK ((1 << PHYS_ADDR_WIDTH) - 1)

; 页表项位字段偏移和掩码
%define P_BIT    0   ; Present 位偏移
%define RW_BIT   1   ; Read/Write 位偏移
%define US_BIT   2   ; User/Supervisor 位偏移
%define PS_BIT  7

; 通用掩码
%define P_MASK   (1 << P_BIT)
%define RW_MASK  (1 << RW_BIT)
%define US_MASK  (1 << US_BIT)
%define PS_MASK  (1 << PS_BIT)

extern KImgphybase 
extern _stack_top
extern kernel_start
extern ap_init
extern bsp_init_gdt_entries
extern bsp_init_gdt_descriptor
extern bsp_init_idt_entries
extern bsp_init_idtr
extern ap_init_patch_idt_rm
extern ap_init_patch_idt_pe
extern ap_init_patch_idt_lm
extern idt_table_rm
extern idt_descriptor_rm
extern idt_descriptor_pe
global realmode_enter_checkpoint

global pemode_enter_checkpoint
global _kernel_Init
global pml4_table_init
global pml5_table_init
global assigned_cr3
global AP_realmode_start
global assigned_processor_id

SECTION .ap_bootstrap_data


; 定义两个check_point结构体
realmode_enter_checkpoint:
    resb check_point_size

pemode_enter_checkpoint:
    resb check_point_size
assigned_processor_id:
    dd 0
assigned_cr3:
    dd 0
ap_tmp_gs_slots:
    times 64 dq 0
SECTION .ap_bootstrap_text

AP_realmode_start:
.ap_start:
align 0x1000
bits 16
cli
    mov eax, [assigned_processor_id]
    mov [realmode_enter_checkpoint+check_point.success_word], eax

    ; 加载GDT
    lgdt [gdt_descriptor]
    lidt [idt_descriptor_rm]
    ; 设置CR0寄存器，启用保护模式
    mov eax, cr0
    or eax, 1                           ; Set PE (Protection Enable) bit
    
    mov cr0, eax
    
    ; 长跳转到保护模式代码段
    jmp 0x8:.ap_start_pe               ; 使用代码段选择子0x08 (代码段在GDT中的偏移)

.ap_start_pe:
bits 32
    ; 设置数据段寄存器
    mov ax, 0x10                        ; 数据段选择子0x10 (数据段在GDT中的偏移)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, init_stack_end
    mov eax, 0xB
    mov ecx, 1
    cpuid
    mov [pemode_enter_checkpoint+check_point.success_word], edx
    lidt [idt_descriptor_pe]
    mov cr0, eax
    mov eax, cr4
    or eax, 1<<5
    and eax, ~(((1 << 12)))    ; 修复位运算表达式语法
    and eax, ~(((1 << 4)))     ; 修复位运算表达式语法
    mov cr4, eax
    mov eax, [assigned_cr3]
    mov cr3, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1<<11)|(1<<8)|1
    wrmsr
    mov eax, cr0
    or eax, (1<<31)|(1<<5)|(1<<16)
    mov cr0, eax
    jmp 0x18:.ap_start_64          ; 使用代码段选择子0x08 (代码段在GDT中的偏移)
; 32个中断处理例程

.ap_start_64:
bits 64
    mov rsp, init_stack_end
    mov rax, wrmsr_func
    mov rdi, 0xC0000101
    mov rsi, ap_tmp_gs_slots
    call rax

    mov rax, wrmsr_func
    mov rdi, 0x277
    mov rsi, 0x0407050600070106
    call rax

    mov rax, cr4
    or rax, 0x668
    mov cr4, rax
    mov rdi, [assigned_processor_id]
    mov [gs:8], rdi
    mov rax, ap_init
    call rax
    ; 添加跳转指令，防止执行下面的数据部分
    
    
    

SECTION .ap_bootstrap_rodata
align 0x1000

; GDT定义
gdt_start:
    dq 0                                ; Nullwan quan bu yi yang descriptor
gdt_code:
    dw 0xFFFF                           ; Limit
    dw 0                                ; Base (low)
    db 0                                ; Base (middle)
    db 10011010b                        ; Access byte (Present, Ring 0, Code, Exec Read)
    db 11001111b                        ; Flags (Granularity 4KB, 32-bit mode)
    db 0                                ; Base (high)
gdt_data:
    dw 0xFFFF                           ; Limit
    dw 0                                ; Base (low)
    db 0                                ; Base (middle)
    db 10010010b                        ; Access byte (Present, Ring 0, Data, Read Write)
    db 11001111b                        ; Flags (Granularity 4KB, 32-bit mode)
    db 0                                ; Base (high)
gdt_longmode_code:
    dw 0xFFFF                           ; Limit
    dw 0                                ; Base (low)
    db 0                                ; Base (middle)
    db 10011000b                        ; Access byte (Present, Ring 0, Code, Exec Read)
    db 10101111b                        ; Flags (Granularity 4KB, 64-bit mode)
    db 0  
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1          ; GDT limit
    dd gdt_start                        ; GDT base address
; 实模式下的中断处理例程标签（用于模拟实模式，实际实模式使用向量跳转表）

SECTION .ap_bootstrap_stack
align 0x1000
times 2048 dq 0
init_stack_end:                         ; 为AP初始栈分配空间
SECTION .data
bsp_init_gs:
    times 64 dq 0
SECTION .text
bits 64
global secure_hlt
global ap_final_work
extern get_current_processor_rsp0
extern wrmsr_func
_kernel_Init:
    mov r15, rdi
    mov rax, wrmsr_func
    mov rdi, 0x277
    mov rsi, 0x0407050600070106
    call rax
    jmp .patch_bsp_idt
.paging_done:
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1<<11)|(1<<8)|1
    wrmsr
    mov rax, cr0
    or rax, (1<<16)
    mov cr0, rax
    
    mov rax, bsp_init_gdt_descriptor
    lgdt [rax]
    mov rax, 0x8
    push rax
    mov rax, .jump_kernel
    push rax
    retfq
.jump_kernel:
    mov rax, bsp_init_idtr
    lidt [rax]
    mov rax, ap_init_patch_idt_rm
    call rax
    mov rax, ap_init_patch_idt_pe
    call rax
    mov rax, ap_init_patch_idt_lm
    call rax
    mov rax, wrmsr_func
    mov rdi, 0xC0000101
    mov rsi, bsp_init_gs
    call rax
    mov rax, kernel_start
    mov rdi, r15
    call  rax
    hlt

.patch_bsp_idt:
extern  div_by_zero_bare_enter;
extern  breakpoint_bare_enter;
extern  nmi_bare_enter;
extern  overflow_bare_enter;
extern  invalid_opcode_bare_enter;
extern  general_protection_bare_enter;
extern  double_fault_bare_enter;
extern  page_fault_bare_enter;
extern  machine_check_bare_enter;
extern  invalid_tss_bare_enter;
extern  simd_floating_point_bare_enter;
extern  virtualization_bare_enter;
extern  timer_bare_enter;
extern  ipi_bare_enter;
extern  asm_panic_bare_enter;
extern  bsp_init_idt_entries;这个IDT表

    ; 先将bsp_init_idt_entries的地址加载到寄存器
    mov rbx, qword bsp_init_idt_entries
    
    ; 设置除零异常 (中断号 0)
    mov rax, qword div_by_zero_bare_enter
    mov word  [rbx + 0*16 + 0], ax      ; offset_low
    shr rax, 16
    mov word  [rbx + 0*16 + 6], ax      ; offset_mid
    shr rax, 16
    mov dword [rbx + 0*16 + 8], eax     ; offset_high
    mov dword [rbx + 0*16 + 12], 0      ; reserved

    ; 设置断点异常 (中断号 3)
    mov rax, qword breakpoint_bare_enter
    mov word  [rbx + 3*16 + 0], ax      ; offset_low
    shr rax, 16
    mov word  [rbx + 3*16 + 6], ax      ; offset_mid
    shr rax, 16
    mov dword [rbx + 3*16 + 8], eax     ; offset_high
    mov dword [rbx + 3*16 + 12], 0      ; reserved

    ; 设置NMI (中断号 2)
    mov rax, qword nmi_bare_enter
    mov word  [rbx + 2*16 + 0], ax      ; offset_low
    shr rax, 16
    mov word  [rbx + 2*16 + 6], ax      ; offset_mid
    shr rax, 16
    mov dword [rbx + 2*16 + 8], eax     ; offset_high
    mov dword [rbx + 2*16 + 12], 0      ; reserved

    ; 设置溢出异常 (中断号 4)
    mov rax, qword overflow_bare_enter
    mov word  [rbx + 4*16 + 0], ax      ; offset_low
    shr rax, 16
    mov word  [rbx + 4*16 + 6], ax      ; offset_mid
    shr rax, 16
    mov dword [rbx + 4*16 + 8], eax     ; offset_high
    mov dword [rbx + 4*16 + 12], 0      ; reserved

    ; 设置无效操作码异常 (中断号 6)
    mov rax, qword invalid_opcode_bare_enter
    mov word  [rbx + 6*16 + 0], ax      ; offset_low
    shr rax, 16
    mov word  [rbx + 6*16 + 6], ax      ; offset_mid
    shr rax, 16
    mov dword [rbx + 6*16 + 8], eax     ; offset_high
    mov dword [rbx + 6*16 + 12], 0      ; reserved

    ; 设置一般保护异常 (中断号 13)
    mov rax, qword general_protection_bare_enter
    mov word  [rbx + 13*16 + 0], ax     ; offset_low
    shr rax, 16
    mov word  [rbx + 13*16 + 6], ax     ; offset_mid
    shr rax, 16
    mov dword [rbx + 13*16 + 8], eax    ; offset_high
    mov dword [rbx + 13*16 + 12], 0     ; reserved

    ; 设置双故障异常 (中断号 8)
    mov rax, qword double_fault_bare_enter
    mov word  [rbx + 8*16 + 0], ax      ; offset_low
    shr rax, 16
    mov word  [rbx + 8*16 + 6], ax      ; offset_mid
    shr rax, 16
    mov dword [rbx + 8*16 + 8], eax     ; offset_high
    mov dword [rbx + 8*16 + 12], 0      ; reserved

    ; 设置页面故障异常 (中断号 14)
    mov rax, qword page_fault_bare_enter
    mov word  [rbx + 14*16 + 0], ax     ; offset_low
    shr rax, 16
    mov word  [rbx + 14*16 + 6], ax     ; offset_mid
    shr rax, 16
    mov dword [rbx + 14*16 + 8], eax    ; offset_high
    mov dword [rbx + 14*16 + 12], 0     ; reserved

    ; 设置机器检查异常 (中断号 18)
    mov rax, qword machine_check_bare_enter
    mov word  [rbx + 18*16 + 0], ax     ; offset_low
    shr rax, 16
    mov word  [rbx + 18*16 + 6], ax     ; offset_mid
    shr rax, 16
    mov dword [rbx + 18*16 + 8], eax    ; offset_high
    mov dword [rbx + 18*16 + 12], 0     ; reserved

    ; 设置无效TSS异常 (中断号 10)
    mov rax, qword invalid_tss_bare_enter
    mov word  [rbx + 10*16 + 0], ax     ; offset_low
    shr rax, 16
    mov word  [rbx + 10*16 + 6], ax     ; offset_mid
    shr rax, 16
    mov dword [rbx + 10*16 + 8], eax    ; offset_high
    mov dword [rbx + 10*16 + 12], 0     ; reserved

    ; 设置SIMD浮点异常 (中断号 19)
    mov rax, qword simd_floating_point_bare_enter
    mov word  [rbx + 19*16 + 0], ax     ; offset_low
    shr rax, 16
    mov word  [rbx + 19*16 + 6], ax     ; offset_mid
    shr rax, 16
    mov dword [rbx + 19*16 + 8], eax    ; offset_high
    mov dword [rbx + 19*16 + 12], 0     ; reserved

    ; 设置虚拟化异常 (中断号 20)
    mov rax, qword virtualization_bare_enter
    mov word  [rbx + 20*16 + 0], ax     ; offset_low
    shr rax, 16
    mov word  [rbx + 20*16 + 6], ax     ; offset_mid
    shr rax, 16
    mov dword [rbx + 20*16 + 8], eax    ; offset_high
    mov dword [rbx + 20*16 + 12], 0     ; reserved

    ; 设置定时器中断 (中断号 0x20)
    mov rax, qword timer_bare_enter
    mov word  [rbx + 0x20*16 + 0], ax   ; offset_low
    shr rax, 16
    mov word  [rbx + 0x20*16 + 6], ax   ; offset_mid
    shr rax, 16
    mov dword [rbx + 0x20*16 + 8], eax  ; offset_high
    mov dword [rbx + 0x20*16 + 12], 0   ; reserved

    ; 设置IPI中断 (中断号 0x21)
    mov rax, qword ipi_bare_enter
    mov word  [rbx + 0x21*16 + 0], ax   ; offset_low
    shr rax, 16
    mov word  [rbx + 0x21*16 + 6], ax   ; offset_mid
    shr rax, 16
    mov dword [rbx + 0x21*16 + 8], eax  ; offset_high
    mov dword [rbx + 0x21*16 + 12], 0   ; reserved
    jmp .paging_done
secure_hlt:
    sti 
    hlt
    jmp secure_hlt

; AP final parking work:
; 1) read current x64_local_processor* from GS slot 0
; 2) use struct offset to load tss.rsp0
; 3) switch to rsp0 and halt with interrupts enabled
ap_final_work:
    cli

    ; reserve 256 bytes first, then align stack for C ABI call
    sub rsp, 256
    mov rdx, rsp
    and rdx, -16
    mov rsp, rdx
    mov rax, get_current_processor_rsp0
    call rax
    test rax, rax
    jz .ap_final_halt_loop
    mov rsp, rax
    sti 
.ap_final_halt_loop:
    hlt
    jmp .ap_final_halt_loop
;todo 这里改成跳入一个extern "C" 入口 ap_norm_start
