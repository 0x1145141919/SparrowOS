; SPDX-License-Identifier: GPL-2.0-only
;
; wheels.asm — 内核基础工具函数（NASM 裸汇编优化版）
;
; 用 x86 硬件字符串指令替代 C 逐字节循环，消除分支预测开销。
; System V AMD64 ABI (Linux/ELF calling convention):
;   1st arg: rdi, 2nd: rsi, 3rd: rdx, 4th: rcx, 5th: r8, 6th: r9
;   return: rax
;
; 叶函数不使用 frame pointer，减少指令数便于审核。

section .text
bits 64

; ================================================================
; size_t strlen_in_kernel(const char *s)
; ================================================================
; 硬件加速: repne scasb 一次扫描整个字符串查找 '\0'。
; 比 C 逐字节循环少 n 次分支预测失败，大字符串差距更明显。
; ================================================================
global strlen_in_kernel
strlen_in_kernel:
    ; rdi = s
    xor     eax, eax            ; al = 0 (搜索 '\0')
    mov     rcx, -1             ; 最大搜索长度
    repne scasb                 ; 逐字节搜索直到匹配
    not     rcx                 ; rcx = 搜索过的字节数（含 '\0'）
    dec     rcx                 ; 减掉 '\0' 本身 = 字符串长度
    mov     rax, rcx            ; 返回值
    ret

; ================================================================
; int strcmp_in_kernel(const char *str1, const char *str2,
;                      uint32_t max_strlen)
; ================================================================
; 硬件加速: repe cmpsb 一次比较多个字节。
; 短字符串（<128 字节）明显快于 C 循环。
; ================================================================
global strcmp_in_kernel
strcmp_in_kernel:
    ; rdi = str1, rsi = str2, rdx = max_strlen
    mov     rcx, rdx            ; max_strlen
    xor     eax, eax
    repe cmpsb                  ; 比较直到不等或 rcx=0
    jne     .diff               ; 发现差异
    ; 相等（要么完全匹配，要么都遇到 '\0'）
    xor     eax, eax
    ret

.diff:
    ; str1[i-1] - str2[i-1]
    movzx   eax, byte [rdi - 1]
    movzx   ecx, byte [rsi - 1]
    sub     eax, ecx
    ret

; ================================================================
; int strncmp_in_kernel(const char *str1, const char *str2,
;                       size_t n)
; ================================================================
; 硬件加速: repe cmpsb 一次比较，rcx = n。
; 相等时返回 0，不等返回差值。
; ================================================================
global strncmp_in_kernel
strncmp_in_kernel:
    ; rdi = str1, rsi = str2, rdx = n
    mov     rcx, rdx            ; n
    test    rcx, rcx
    jz      .equal              ; n == 0 → 返回 0

    xor     eax, eax
    repe cmpsb                  ; 比较最多 n 个字节
    jne     .diff               ; 发现差异

.equal:
    xor     eax, eax
    ret

.diff:
    movzx   eax, byte [rdi - 1]
    movzx   ecx, byte [rsi - 1]
    sub     eax, ecx
    ret

; ================================================================
; void ksystemramcpy(void *src, void *dest, size_t length)
; ================================================================
; C 版本: 逐字节循环或 memcpy → 分支预测惩罚。
;
; 硬件加速: rep movsb 一次复制整块内存。
; 支持重叠区域: src < dest 时从后往前复制 (std)。
;
; System V: rdi = src, rsi = dest, rdx = length
; ================================================================
global ksystemramcpy
ksystemramcpy:
    mov     rcx, rdx            ; length → count
    test    rcx, rcx
    jz      .zero

    cmp     rdi, rsi            ; src < dest?
    jae     .forward

    ; src < dest: 检查重叠 src + length > dest
    mov     r8, rdi
    add     r8, rcx             ; r8 = src + length
    cmp     r8, rsi             ; src + length > dest?
    jbe     .forward            ; 无重叠 → forward

    ; 有重叠且 src < dest → 从后往前复制
    xchg    rdi, rsi            ; rdi = dest, rsi = src
    add     rsi, rcx
    dec     rsi                 ; rsi = src + length - 1
    add     rdi, rcx
    dec     rdi                 ; rdi = dest + length - 1
    std                         ; 方向 = 递减
    rep movsb
    cld                         ; 恢复方向
    ret

.forward:
    xchg    rdi, rsi            ; rdi = dest, rsi = src
    cld
    rep movsb

.zero:
    ret

; ================================================================
; void ksetmem_8(void *ptr, uint8_t value, uint64_t size_in_byte)
; void ksetmem_16(void *ptr, uint16_t value, uint64_t size_in_byte)
; void ksetmem_32(void *ptr, uint32_t value, uint64_t size_in_byte)
; void ksetmem_64(void *ptr, uint64_t value, uint64_t size_in_byte)
; ================================================================
; 硬件加速: rep stosb/stosw/stosl/stosq 一次填充整块内存。
; tail 部分（未对齐尾字节）用 rep stosb 兜底。
; ================================================================

global ksetmem_8
ksetmem_8:
    ; rdi = ptr, sil = value, rdx = size_in_byte
    mov     eax, esi            ; al = value
    mov     rcx, rdx            ; count
    cld
    rep stosb
    ret

global ksetmem_16
ksetmem_16:
    ; rdi = ptr, si = value, rdx = size_in_byte
    mov     eax, esi            ; ax = value
    mov     rcx, rdx
    shr     rcx, 1              ; count = size / 2
    cld
    rep stosw
    ; tail: 多余 1 字节
    test    dl, 1
    jz      .done_16
    stosb                       ; 存低字节
.done_16:
    ret

global ksetmem_32
ksetmem_32:
    ; rdi = ptr, esi = value, rdx = size_in_byte
    mov     eax, esi            ; eax = value
    mov     rcx, rdx
    shr     rcx, 2              ; count = size / 4
    cld
    rep stosd
    ; tail: 多余 0-3 字节
    mov     rcx, rdx
    and     ecx, 3
    jz      .done_32
    rep stosb                   ; al = value 低字节
.done_32:
    ret

global ksetmem_64
ksetmem_64:
    ; rdi = ptr, rsi = value, rdx = size_in_byte
    mov     rax, rsi            ; rax = value
    mov     rcx, rdx
    shr     rcx, 3              ; count = size / 8
    cld
    rep stosq
    ; tail: 多余 0-7 字节
    mov     rcx, rdx
    and     ecx, 7
    jz      .done_64
    rep stosb                   ; al = value 低字节
.done_64:
    ret

; ================================================================
; void *memset(void *s, int c, size_t n) — 替代 gnu-efi
; void *memcpy(void *dest, const void *src, size_t n) — 替代 gnu-efi
; ================================================================
; 编译器喜欢主动内联调用这些符号。纯 asm 展开消除 C 包装器的调用帧开销。
; ================================================================
global memset
memset:
    ; rdi = s, esi = c, rdx = n
    mov     r8, rdi             ; 保存原始指针用于返回值
    mov     eax, esi            ; al = fill byte
    mov     rcx, rdx            ; count
    cld
    rep stosb
    mov     rax, r8             ; 返回原始 s
    ret

global memcpy
memcpy:
    ; rdi = dest, rsi = src, rdx = n
    mov     r8, rdi             ; 保存 dest 用于返回值
    mov     rcx, rdx            ; count
    cld
    rep movsb
    mov     rax, r8             ; 返回原始 dest
    ret

; ================================================================
; int memcmp(const void *s1, const void *s2, size_t n)
; ================================================================
; 硬件加速: repe cmpsb 一次比较整块内存，遇不等即停。
; 返回差值 (s1[i] - s2[i])，相等返回 0。
; ================================================================
global memcmp
memcmp:
    ; rdi = s1, rsi = s2, rdx = n
    mov     rcx, rdx            ; n
    test    rcx, rcx
    jz      .equal

    xor     eax, eax
    repe cmpsb                  ; 比较直到不等或 rcx=0
    jne     .diff

.equal:
    xor     eax, eax
    ret

.diff:
    movzx   eax, byte [rdi - 1]
    movzx   ecx, byte [rsi - 1]
    sub     eax, ecx
    ret

; ================================================================
; void __kspace_stack_chk_fail(void)
; ================================================================
; 栈保护失败 → 触发 INT 0xC（堆栈段异常向量）。
; IDT 中 0xC 处理程序应执行内核 panic。
; ================================================================
global __kspace_stack_chk_fail
__kspace_stack_chk_fail:
    int     0xc
    ret
