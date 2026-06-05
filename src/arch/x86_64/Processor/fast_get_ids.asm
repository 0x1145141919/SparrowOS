; ===================================================================
; fast_get_processor_id / fast_get_x2apic_id — GS-relative 单指令读取
;
; C 签名:
;   uint32_t fast_get_processor_id(void);
;   uint32_t fast_get_x2apic_id(void);
;
; GS 段基址指向当前核的 gs_complex_t 实例。
; slots[1] 编码: [31:0] = logical processor id, [63:32] = x2APIC id
; slot[1] 偏移 = 1 * sizeof(uint64_t) = 8
;
; 不需要保存任何寄存器（仅读 GS 段 + 纯寄存器操作）。
; ===================================================================

global fast_get_processor_id
global fast_get_x2apic_id

section .text

fast_get_processor_id:
    mov rax, [gs:8]
    and eax, 0xFFFFFFFF
    ret

fast_get_x2apic_id:
    mov rax, [gs:8]
    shr rax, 32
    ret
