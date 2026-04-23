SECTION .text
global atoimc_kthread_load
bits 64
%define kthread_call_ivec 226
%define KTHREAD_CALL_EXIT   0
%define KTHREAD_CALL_SLEEP  1
%define KTHREAD_CALL_YIELD  2
%define KTHREAD_CALL_WAIT   3
%define KTHREAD_CALL_BLOCK  4
%define KTHREAD_CALL_BLOCK_QUEUE  5
%define KTHREAD_CALL_BLOCK_QUEUE_IF_EQUAL  6
atoimc_kthread_load:
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
block_queue_if_equal:
KTREAD_CALL_TEMPLATE KTHREAD_CALL_BLOCK_QUEUE_IF_EQUAL
