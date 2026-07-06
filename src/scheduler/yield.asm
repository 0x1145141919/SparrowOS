SECTION .text
global idt_style_load
bits 64
%define kthread_call_ivec 226
%define KTHREAD_CALL_EXIT   0
%define KTHREAD_CALL_SLEEP  1
%define KTHREAD_CALL_YIELD  2
%define KTHREAD_CALL_WAIT   3
%define KTHREAD_CALL_BLOCK  4
%define KTHREAD_CALL_BLOCK_QUEUE  5
%define KTHREAD_CALL_BLOCK_QUEUE_IF_EQUAL  6
idt_style_load:
    ;这里还要根据特权级是不是ring 3进行swapgs
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
    add rsp, 8
    iretq
global fred_uctx_load
fred_uctx_load:
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
    eretu
    ud2
global fred_pctx_load
fred_pctx_load:
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
    erets
    ud2
global allkthread_true_enter
allkthread_true_enter:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    mov rcx, r8
    mov r8, r9
    call rax
    mov rdi, rax
    mov rax, KTHREAD_CALL_EXIT
    int kthread_call_ivec
    ud2

%macro KTREAD_CALL_TEMPLATE 1
mov rax, %1
int kthread_call_ivec
ret 
nop
%endmacro

global kthread_yield
kthread_yield:
KTREAD_CALL_TEMPLATE KTHREAD_CALL_YIELD


global kthread_exit
kthread_exit:
    mov rax, KTHREAD_CALL_EXIT
    int kthread_call_ivec
    ud2


global kthread_self_blocked
kthread_self_blocked:
KTREAD_CALL_TEMPLATE KTHREAD_CALL_BLOCK

global kthread_sleep
kthread_sleep:
KTREAD_CALL_TEMPLATE KTHREAD_CALL_SLEEP

global kthread_wait_truly_wait
kthread_wait_truly_wait:
KTREAD_CALL_TEMPLATE KTHREAD_CALL_WAIT


global block_queue
block_queue:
KTREAD_CALL_TEMPLATE KTHREAD_CALL_BLOCK_QUEUE


global block_if_equal
block_if_equal:
KTREAD_CALL_TEMPLATE KTHREAD_CALL_BLOCK_QUEUE_IF_EQUAL

global gs_offsetptr_dumper
gs_offsetptr_dumper:
    rdgsbase rax
    add rax, rdi
    ret
global common_idle
common_idle:
.loop_idle:
    sti
    hlt
    mov rax, KTHREAD_CALL_YIELD
    int kthread_call_ivec
    jmp .loop_idle


