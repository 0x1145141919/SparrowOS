; ===================================================================
; cmpxchg16b — 原子 16 字节比较交换
;
; C 签名:
;   bool cmpxchg16b(void* ptr, void* expected, const void* desired);
;
; 语义:
;   若 *ptr == *expected, 则将 *desired 写入 *ptr 并返回 true.
;   否则将 *ptr 当前值写入 *expected 并返回 false.
;
; 输入:
;   RDI — ptr      目标内存地址 (16B 对齐)
;   RSI — expected 指向期望值的指针 (16B, 失败时被当前值覆盖)
;   RDX — desired  指向目标值的指针 (16B, 只读)
;
; 输出:
;   AL  — true(1)  成功, false(0) 失败
; ===================================================================

global cmpxchg16b
section .text
cmpxchg16b:
    ; 保存 callee-saved RBX
    push rbx

    ; ── 装填 cmpxchg16b 操作数 ──
    ;   RDX:RAX = expected (从 [RSI] 加载)
    ;   RCX:RBX = desired  (从 [RDX] 加载)
    mov rax, [rsi]          ; expected.lo
    push rsi                ; 暂存 expected 指针 (失败回写用)
    mov rbx, [rdx]          ; desired.lo
    mov rcx, [rdx + 8]      ; desired.hi
    mov rdx, [rsi + 8]      ; expected.hi

    lock cmpxchg16b [rdi]
    jz .success

    ; ── 失败: 更新 *expected ← 当前内存值 ──
    pop rsi                 ; 恢复 expected 指针
    mov [rsi],     rax      ; 写回当前值低 64 位
    mov [rsi + 8], rdx      ; 写回当前值高 64 位
    xor eax, eax            ; return false
    pop rbx
    ret

.success:
    ; ── 成功: 丢弃暂存的 expected 指针 ──
    add rsp, 8              ; pop 跳过暂存的 RSI
    mov eax, 1              ; return true
    pop rbx
    ret
