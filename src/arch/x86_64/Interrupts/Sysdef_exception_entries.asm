section .text
%define GP_GPR_COUNT 16
%define GP_GPR_BYTES (GP_GPR_COUNT * 8)

; 定义中断上下文魔数宏
%define INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC 0x8000000000000000    ; 无错误码的中断上下文魔数
%define INTERRUPT_CONTEXT_SPECIFY_MAGIC    0x8000000000000001    ; 有错误码的中断上下文魔数

; 除零异常处理入口
global div_by_zero_bare_enter
extern div_by_zero_cpp_enter

div_by_zero_bare_enter:
    sub rsp, 8
    push rbp                    ; 保存当前栈帧
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
    mov rax, 0x0 
    push rax
    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC ; 给interrupt_context_specify_magic填写字段，表明无错误码
    mov qword [rax], rbx
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, div_by_zero_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8
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
    add rsp, 8
    iretq                       ; 中断返回


; 断点异常处理入口
global breakpoint_bare_enter
extern breakpoint_cpp_enter

breakpoint_bare_enter:
    sub rsp, 8
    push rbp                    ; 保存当前栈帧
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
    mov rax, 0x03 
    push rax
    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC ; 给interrupt_context_specify_magic填写字段，表明无错误码
    mov qword [rax], rbx
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, breakpoint_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8
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
    add rsp, 8
    iretq                       ; 中断返回



; NMI异常处理入口
global nmi_bare_enter
extern nmi_cpp_enter

nmi_bare_enter:
    sub rsp, 8
    push rbp                    ; 保存当前栈帧
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
    mov rax, 0x02 
    push rax
    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC ; 给interrupt_context_specify_magic填写字段，表明无错误码
    mov qword [rax], rbx
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, nmi_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8
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
    add rsp, 8
    iretq                       ; 中断返回


; 溢出异常处理入口
global overflow_bare_enter
extern overflow_cpp_enter

overflow_bare_enter:
    sub rsp, 8
    push rbp                    ; 保存当前栈帧
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
    mov rax, 0x04 
    push rax
    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC ; 给interrupt_context_specify_magic填写字段，表明无错误码
    mov qword [rax], rbx
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, overflow_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8
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
    add rsp, 8
    iretq                       ; 中断返回




; 无效操作码异常处理入口
global invalid_opcode_bare_enter
extern invalid_opcode_cpp_enter

invalid_opcode_bare_enter:
    sub rsp, 8
    push rbp                    ; 保存当前栈帧
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
    mov rax, 0x06 
    push rax
    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC ; 给interrupt_context_specify_magic填写字段，表明无错误码
    mov qword [rax], rbx
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, invalid_opcode_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8
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
    add rsp, 8
    iretq                       ; 中断返回


global general_protection_bare_enter
extern general_protection_cpp_enter

general_protection_bare_enter:
    ; 进入时，CPU 已经做了：
    ;   push ss        (可能)
    ;   push rsp       (可能)
    ;   push rflags
    ;   push cs
    ;   push rip
    ;   push errcode  <-- #GP 有

    sub rsp, 8
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


    ; magic
    mov rax, 0xD|(1<<32)          ;向量号|（是否有错误码<<32）
    push rax

    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_MAGIC ; 给interrupt_context_specify_magic填写字段，表明有错误码
    mov qword [rax], rbx
    ; rdi = struct x64_context*
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, general_protection_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8

    ; 回收 magic + interrupt_context_specify_magic
    add rsp, 16

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
    add rsp, 8
    iretq


; 双重错误异常处理入口（带错误码）
global double_fault_bare_enter
extern double_fault_cpp_enter

double_fault_bare_enter:
    ; 进入时，CPU 已经做了：
    ;   push ss        (可能)
    ;   push rsp       (可能)
    ;   push rflags
    ;   push cs
    ;   push rip
    ;   push errcode  <-- #DF 有

    sub rsp, 8
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


    ; magic
    mov rax, 0x08|(1<<32)          ;向量号|（是否有错误码<<32）
    push rax

    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_MAGIC ; 给interrupt_context_specify_magic填写字段，表明有错误码
    mov qword [rax], rbx
    ; rdi = struct x64_context*
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, double_fault_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8

    ; 回收 magic + interrupt_context_specify_magic
    add rsp, 16

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
    add rsp, 8
    iretq



; 页错误异常处理入口（带错误码）
global page_fault_bare_enter
extern page_fault_cpp_enter

page_fault_bare_enter:
    ; 进入时，CPU 已经做了：
    ;   push ss        (可能)
    ;   push rsp       (可能)
    ;   push rflags
    ;   push cs
    ;   push rip
    ;   push errcode  <-- #PF 有

    sub rsp, 8
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


    ; magic
    mov rax, 0x0E|(1<<32)          ;向量号|（是否有错误码<<32）
    push rax

    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_MAGIC ; 给interrupt_context_specify_magic填写字段，表明有错误码
    mov qword [rax], rbx
    ; rdi = struct x64_context*
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, page_fault_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8

    ; 回收 magic + interrupt_context_specify_magic
    add rsp, 16

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
    add rsp, 8
    iretq



; 机器检查异常处理入口
global machine_check_bare_enter
extern machine_check_cpp_enter

machine_check_bare_enter:
    sub rsp, 8
    push rbp                    ; 保存当前栈帧
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
    mov rax, 0x12 
    push rax
    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC ; 给interrupt_context_specify_magic填写字段，表明无错误码
    mov qword [rax], rbx
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, machine_check_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8
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
    add rsp, 8
    iretq                       ; 中断返回


; 无效TSS异常处理入口（带错误码）
global invalid_tss_bare_enter
extern invalid_tss_cpp_enter

invalid_tss_bare_enter:
    ; 进入时，CPU 已经做了：
    ;   push ss        (可能)
    ;   push rsp       (可能)
    ;   push rflags
    ;   push cs
    ;   push rip
    ;   push errcode  <-- #TS 有

    sub rsp, 8
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


    ; magic
    mov rax, 0x0A|(1<<32)          ;向量号|（是否有错误码<<32）
    push rax

    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_MAGIC ; 给interrupt_context_specify_magic填写字段，表明有错误码
    mov qword [rax], rbx
    ; rdi = struct x64_context*
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, invalid_tss_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8

    ; 回收 magic + interrupt_context_specify_magic
    add rsp, 16

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
    add rsp, 8
    iretq


; SIMD浮点异常处理入口
global simd_floating_point_bare_enter
extern simd_floating_point_cpp_enter

simd_floating_point_bare_enter:
    sub rsp, 8
    push rbp                    ; 保存当前栈帧
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
    mov rax, 0x13 
    push rax
    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC ; 给interrupt_context_specify_magic填写字段，表明无错误码
    mov qword [rax], rbx
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, simd_floating_point_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8
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
    add rsp, 8
    iretq                       ; 中断返回


; 虚拟化异常处理入口（带错误码）
global virtualization_bare_enter
extern virtualization_cpp_enter

virtualization_bare_enter:
    ; 进入时，CPU 已经做了：
    ;   push ss        (可能)
    ;   push rsp       (可能)
    ;   push rflags
    ;   push cs
    ;   push rip
    ;   push errcode  <-- #VE 有

    sub rsp, 8
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


    ; magic
    mov rax, 0x14|(1<<32)          ;向量号|（是否有错误码<<32）
    push rax

    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_MAGIC ; 给interrupt_context_specify_magic填写字段，表明有错误码
    mov qword [rax], rbx
    ; rdi = struct x64_context*
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, virtualization_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8

    ; 回收 magic + interrupt_context_specify_magic
    add rsp, 16

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
    add rsp, 8
    iretq


; 定时器中断处理入口
global timer_bare_enter
extern timer_cpp_enter

timer_bare_enter:
    sub rsp, 8
    push rbp                    ; 保存当前栈帧
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
    mov rax, 224 
    push rax
    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC ; 给interrupt_context_specify_magic填写字段，表明无错误码
    mov qword [rax], rbx
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, timer_cpp_enter
    call rax
    pop rsp ;栈自动回落
    add rsp, 8
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
    add rsp, 8
    iretq                       ; 中断返回


; IPI中断处理入口（带错误码）
global ipi_bare_enter
extern ipi_cpp_enter

ipi_bare_enter:
    sub rsp, 8
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


    ; magic
    mov rax, 240          ;向量号|（是否有错误码<<32）
    push rax

    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC ; 给interrupt_context_specify_magic填写字段，表明有错误码
    mov qword [rax], rbx
    ; rdi = struct x64_context*
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, ipi_cpp_enter
    call rax

    pop rsp ;栈自动回落
    add rsp, 8
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
    add rsp, 8
    iretq
global kthread_call_bare_enter
extern kthread_call_cpp_enter
kthread_call_bare_enter:
    sub rsp, 8
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


    ; magic
    mov rax, 226          ;向量号|（是否有错误码<<32）
    push rax

    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC ; 给interrupt_context_specify_magic填写字段，表明有错误码
    mov qword [rax], rbx
    ; rdi = struct x64_context*
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, kthread_call_cpp_enter
    call rax

    pop rsp ;栈自动回落
    add rsp, 8
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
    add rsp, 8
    iretq
    ; asm_panic中断处理入口（带错误码）
global asm_panic_bare_enter
extern asm_panic_cpp_enter

asm_panic_bare_enter:
    sub rsp, 8
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


    ; magic
    mov rax, 225          ;向量号|（是否有错误码<<32）
    push rax

    mov rax, rsp
    add rax, GP_GPR_BYTES
    mov rbx, INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC ; 给interrupt_context_specify_magic填写字段，表明有错误码
    mov qword [rax], rbx
    ; rdi = struct x64_context*
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, asm_panic_cpp_enter
    call rax

    pop rsp ;栈自动回落
    add rsp, 8

    ; 回收 magic + interrupt_context_specify_magic
    add rsp, 16

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
    add rsp, 8
    iretq