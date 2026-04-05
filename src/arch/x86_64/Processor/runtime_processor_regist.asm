global runtime_processor_regist
section .text
runtime_processor_regist:
    ; rdi = &load_resources_struct

    ; ---- load GDT ----
    mov rax, [rdi + 0x00]       ; gdtr*
    lgdt [rax]

    ; ---- reload CS ----
    mov ax, [rdi + 0x10]        ; K_CS_selector (16-bit)
    push rax
    mov rax, .load_cs_finish
    push rax
    retfq

.load_cs_finish:
    ; ---- reload data segments ----
    mov ax, [rdi + 0x18]        ; K_DS_selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- load TSS ----
    mov ax, [rdi + 0x20]
    ltr ax

    ; ---- load IDT ----
    mov rax, [rdi + 0x08]       ; idtr*
    lidt [rax]

    ret