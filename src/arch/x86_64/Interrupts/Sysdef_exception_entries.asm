; ===================================================================
; 异常入口 — IDT vectors 0–31（每个独立 bare_enter 符号 + C handler）
; 向量解复用器 — IDT vectors 32–255（vec_demux_table trampoline）
; FRED 入口 — fred_user_enter / fred_supervisor_entry
; ===================================================================

; --- 异常入口宏（无错误码） ---
%macro EXCEPTION_ENTRY 2
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
    mov rax, [rsp + 15 * 8]     ; CS at offset 120 (15 GPR)
    test al, 3
    jz %%skip_gs
    swapgs
%%skip_gs:
    mov rdi, rsp
    mov rax, rsp
    and rsp, -16
    sub rsp, 8
    push rax
    mov rax, %2
    call rax
    pop rsp
    ; reverse swapgs if needed
    mov rax, [rsp + 15 * 8]     ; CS
    test al, 3
    jz %%skip_gs_back
    swapgs
%%skip_gs_back:
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

; --- 异常入口宏（带错误码） ---
%macro EXCEPTION_ENTRY_WITH_ERRCODE 2
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
    mov rax, [rsp + 15 * 8 + 8] ; CS at offset 120 + 8 (errcode)
    test al, 3
    jz %%skip_gs
    swapgs
%%skip_gs:
    mov rdi, rsp
    mov rax, rsp
    and rsp, -16
    sub rsp, 8
    push rax
    mov rax, %2
    call rax
    pop rsp
    ; reverse swapgs if needed
    mov rax, [rsp + 15 * 8 + 8] ; CS
    test al, 3
    jz %%skip_gs_back
    swapgs
%%skip_gs_back:
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
    add rsp, 8                  ; 弹出硬件压入的错误码
    iretq
%endmacro

; ========== IDT vectors 0–31: per-exception entries ==========

global div_by_zero_bare_enter
extern div_by_zero_cpp_enter
div_by_zero_bare_enter:
    EXCEPTION_ENTRY 0, div_by_zero_cpp_enter

global breakpoint_bare_enter
extern breakpoint_cpp_enter
breakpoint_bare_enter:
    EXCEPTION_ENTRY 3, breakpoint_cpp_enter

global nmi_bare_enter
extern nmi_cpp_enter
nmi_bare_enter:
    EXCEPTION_ENTRY 2, nmi_cpp_enter

global overflow_bare_enter
extern overflow_cpp_enter
overflow_bare_enter:
    EXCEPTION_ENTRY 4, overflow_cpp_enter

global invalid_opcode_bare_enter
extern invalid_opcode_cpp_enter
invalid_opcode_bare_enter:
    EXCEPTION_ENTRY 6, invalid_opcode_cpp_enter

global general_protection_bare_enter
extern general_protection_cpp_enter
general_protection_bare_enter:
    EXCEPTION_ENTRY_WITH_ERRCODE 0x0D, general_protection_cpp_enter

global double_fault_bare_enter
extern double_fault_cpp_enter
double_fault_bare_enter:
    EXCEPTION_ENTRY_WITH_ERRCODE 0x08, double_fault_cpp_enter

global page_fault_bare_enter
extern page_fault_cpp_enter
page_fault_bare_enter:
    EXCEPTION_ENTRY_WITH_ERRCODE 0x0E, page_fault_cpp_enter

global machine_check_bare_enter
extern machine_check_cpp_enter
machine_check_bare_enter:
    EXCEPTION_ENTRY 18, machine_check_cpp_enter

global invalid_tss_bare_enter
extern invalid_tss_cpp_enter
invalid_tss_bare_enter:
    EXCEPTION_ENTRY_WITH_ERRCODE 0x0A, invalid_tss_cpp_enter

global simd_floating_point_bare_enter
extern simd_floating_point_cpp_enter
simd_floating_point_bare_enter:
    EXCEPTION_ENTRY 19, simd_floating_point_cpp_enter

global virtualization_bare_enter
extern virtualization_cpp_enter
virtualization_bare_enter:
    EXCEPTION_ENTRY_WITH_ERRCODE 20, virtualization_cpp_enter

; ========== FRED 入口 ==========

global fred_user_enter
extern fred_user_cpp_enter
extern fred_base
align 4096
fred_base:
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
    mov rax, rsp
    and rsp, -16
    sub rsp, 8
    push rax
    mov rax, fred_user_cpp_enter
    call rax
    pop rsp
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

global fred_supervisor_entry
extern fred_supervisor_cpp_entry
fred_supervisor_entry:
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
    mov rax, rsp
    and rsp, -16
    sub rsp, 8
    push rax
    mov rax, fred_supervisor_cpp_entry
    call rax
    pop rsp
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

; ===================================================================
; 向量解复用器 (Vector Demux) — 16B trampoline 表 + 公共主体
; 224 个入口，每个 16 字节。IDT 地址 = vec_demux_table + (vec-32)*16
; 栈布局 (C 入口收到时)：
;   [rax..rbp]  ← RSP, 15 GPR × 8 = 120B (common push)
;   [vec]                       8B (trampoline push qword N)
;   [rip..ss]                  40B (硬件压入)
; C 入口: vec_demux_entry(x64_vec_demux_frame*) — 内部转标准帧
; ===================================================================

extern idt_vec_demux_entry
global vec_demux_table
vec_demux_table:
%assign __vec 32
%rep 224
    push strict qword __vec   ; 5B — 向量号入栈 (68 imm32)
    jmp strict near vec_demux_common ; 5B — near jmp (E9 rel32)
    times 6 nop                ; pad to 16B
%assign __vec __vec + 1
%endrep

; --- 距栈顶偏移 (15 GPR) ---
VEC_OFFSET equ 15 * 8          ; vec 在 15 个 GPR 之下

vec_demux_common:
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
    ; CS = [rsp + VEC_OFFSET + vec(8) + rip(8)]
    mov rax, [rsp + VEC_OFFSET + 16]
    test al, 3
    jz .skip_gs
    swapgs
.skip_gs:
    mov rdi, rsp               ; rdi = x64_vec_demux_frame*
    mov rax, rsp
    and rsp, -16
    sub rsp, 8
    push rax
    mov rax, idt_vec_demux_entry
    call rax
    pop rsp
    mov rax, [rsp + VEC_OFFSET + 16]   ; CS
    test al, 3
    jz .skip_gs_back
    swapgs
.skip_gs_back:
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
    add rsp, 8                 ; 弹出 trampoline push 的 vec
    iretq
global cacheline_wait
cacheline_wait:
    ; bool cacheline_wait(void* addr);
    ; UMONITOR + UMWAIT, 返回 false=store 唤醒, true=超时/其他
    umonitor rdi
    xor     eax, eax
    xor     edx, edx
    not     rax             ; EDX:EAX = ~0ULL → 忽略 UMWAIT 指令级 deadline
    not     rdx             ;   实际时限由 IA32_UMWAIT_CONTROL (50μs) 约束
    xor     ecx, ecx        ; ECX=0 → C0.2 状态
    umwait  ecx
    setc    al              ; CF→AL (0=store, 1=timeout)
    movzx   eax, al
    ret
    
