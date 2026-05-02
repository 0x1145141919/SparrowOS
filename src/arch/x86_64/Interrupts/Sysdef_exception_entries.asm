section .text
%define GP_GPR_COUNT 16
%define GP_GPR_BYTES (GP_GPR_COUNT * 8)

; 定义中断上下文魔数宏
%define INTERRUPT_CONTEXT_SPECIFY_NO_MAGIC 0x8000000000000000    ; 无错误码的中断上下文魔数
%define INTERRUPT_CONTEXT_SPECIFY_MAGIC    0x8000000000000001    ; 有错误码的中断上下文魔数
%macro EXCEPTION_ENTRY 2

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
    ; swapgs if from user mode
    mov rax, [rsp + GP_GPR_BYTES]   ; CS at GP_GPR_BYTES (no errcode)
    test al, 3
    jz %%skip_gs_ee
    swapgs
%%skip_gs_ee:
    mov rax, rsp
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, %2
    call rax
    pop rsp ;栈自动回落
    ; reverse swapgs if needed (rax = C return value, not a saved reg)
    mov rax, [rsp + GP_GPR_BYTES]   ; CS (no errcode)
    test al, 3
    jz %%skip_gs_back_ee
    swapgs
%%skip_gs_back_ee:
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
    iretq                       ; 中断返回
%endmacro
%macro EXCEPTION_ENTRY_WITH_ERRCODE 2
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
    ; swapgs if from user mode
    mov rax, [rsp + GP_GPR_BYTES + 8]   ; CS at GP_GPR_BYTES+8 (errcode)
    test al, 3
    jz %%skip_gs_errcode
    swapgs
%%skip_gs_errcode:
    mov rax, rsp
    mov rdi, rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, %2
    call rax
    pop rsp ;栈自动回落
    ; reverse swapgs if needed (rax = C return value, not a saved reg)
    mov rax, [rsp + GP_GPR_BYTES + 8]   ; CS (errcode)
    test al, 3
    jz %%skip_gs_back_errcode
    swapgs
%%skip_gs_back_errcode:
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
    iretq                       ; 中断返回
%endmacro
; 除零异常处理入口
global div_by_zero_bare_enter
extern div_by_zero_cpp_enter

div_by_zero_bare_enter:
EXCEPTION_ENTRY 0, div_by_zero_cpp_enter                   ; 中断返回


; 断点异常处理入口
global breakpoint_bare_enter
extern breakpoint_cpp_enter

breakpoint_bare_enter:
EXCEPTION_ENTRY 3, breakpoint_cpp_enter



; NMI异常处理入口
global nmi_bare_enter
extern nmi_cpp_enter

nmi_bare_enter:
    
EXCEPTION_ENTRY 2, nmi_cpp_enter


; 溢出异常处理入口
global overflow_bare_enter
extern overflow_cpp_enter

overflow_bare_enter:
    
EXCEPTION_ENTRY 4, overflow_cpp_enter




; 无效操作码异常处理入口
global invalid_opcode_bare_enter
extern invalid_opcode_cpp_enter

invalid_opcode_bare_enter:
EXCEPTION_ENTRY 6, invalid_opcode_cpp_enter

global general_protection_bare_enter
extern general_protection_cpp_enter

general_protection_bare_enter:
EXCEPTION_ENTRY_WITH_ERRCODE 0x0D, general_protection_cpp_enter


; 双重错误异常处理入口（带错误码）
global double_fault_bare_enter
extern double_fault_cpp_enter

double_fault_bare_enter:

EXCEPTION_ENTRY_WITH_ERRCODE 0x08, double_fault_cpp_enter



; 页错误异常处理入口（带错误码）
global page_fault_bare_enter
extern page_fault_cpp_enter

page_fault_bare_enter:

EXCEPTION_ENTRY_WITH_ERRCODE 0x0E, page_fault_cpp_enter



; 机器检查异常处理入口
global machine_check_bare_enter
extern machine_check_cpp_enter

machine_check_bare_enter:

EXCEPTION_ENTRY 18, machine_check_cpp_enter


; 无效TSS异常处理入口（带错误码）
global invalid_tss_bare_enter
extern invalid_tss_cpp_enter

invalid_tss_bare_enter:
 EXCEPTION_ENTRY_WITH_ERRCODE 0x0A, invalid_tss_cpp_enter
; SIMD浮点异常处理入口
global simd_floating_point_bare_enter
extern simd_floating_point_cpp_enter

simd_floating_point_bare_enter:

EXCEPTION_ENTRY 19, simd_floating_point_cpp_enter

; 虚拟化异常处理入口（带错误码）
global virtualization_bare_enter
extern virtualization_cpp_enter

virtualization_bare_enter:
EXCEPTION_ENTRY_WITH_ERRCODE 20, virtualization_cpp_enter

global fred_user_enter
extern fred_user_cpp_enter
align 4096
fred_base:
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
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, fred_user_cpp_enter
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
    eretu
    times (256 - ($ - fred_base)) db 0xcc
    
global fred_supervisor_entry:
extern fred_supervisor_cpp_entry:
fred_supervisor_entry:
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
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rax, fred_supervisor_cpp_entry
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
    erets
    ud2
extern all_vec_delivery
%macro VEC_DELIVERY_ENTRY 1
;%1是向量号
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
    ; swapgs if from user mode
    mov rax, [rsp + GP_GPR_BYTES]   ; CS at GP_GPR_BYTES (no errcode)
    test al, 3
    jz %%skip_gs_vec
    swapgs
%%skip_gs_vec:
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rsi, %1
    mov rax, all_vec_delivery
    call rax

    pop rsp ;栈自动回落
    ; reverse swapgs if needed (rax = C return value, not a saved reg)
    mov rax, [rsp + GP_GPR_BYTES]   ; CS (no errcode)
    test al, 3
    jz %%skip_gs_back_vec
    swapgs
%%skip_gs_back_vec:
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
; 批量生成向量32~255的向量递送入口
; 符号通过 kptrace 符号表名字匹配注册，无需 extern
%assign __vec 32
%rep 224
global vec_delivery_%+__vec
global vec_delivery_%+__vec%+_bare_enter
vec_delivery_%+__vec%+_bare_enter:
    VEC_DELIVERY_ENTRY __vec
%assign __vec __vec + 1
%endrep