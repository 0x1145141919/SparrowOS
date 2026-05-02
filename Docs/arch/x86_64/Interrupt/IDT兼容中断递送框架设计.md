该从哪里讲起呢？该从FRED的机制开始吧，自从我看了FRED的机制后对IDT的机制是越看越他妈的不顺眼，一个中断入口只能绑定中断程序指针信息，不带任何额外参数。但是细想实则不然，我可以用256个中断入口，每个入口的高级语言入口其实可以弹入不同的向量信息，实则可以实现完全的向量号递送，类似FRED的机制。
但是，IDT这个设计是完全落后的，无法实现对exception和interrupt/软中断的区分，由是只考虑有对32~255的软中断向量+硬件中断向量递送纳入这套体系
接下来是架构设计：
1.汇编层：
global all_vec_delivery
%macro INTERRUPT_ENTRY_WITH_ERRCODE 1
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
    mov rdi, rsp
    mov rax, rsp            ; 保存原 rsp
    and rsp, -16            ; 对齐到 16
    sub rsp, 8              ; 为 call 的返回地址预留
    push rax                ; 保存原 rsp（现在 rsp % 16 == 0）
    mov rsi, %1
    mov rax, all_vec_delivery
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
    iretq
%endmacro
生成，其唯一的工作时保全上下文，并传入中断向量信息
2.高级语言入口：
IDT的向量设计完全让硬件异常，外部中断，INT X挤在一个空间，而exception各自的一手信息不一样，页错误不但有错误码还有错误线性地址，#DE则没有，遂不再这个向量递送机制之下，但是软中断和硬中断的则完全由我设计掌握，遂要进行递送
涉及到两个数据结构，全局的，编译链接时确定的，静态的，soft_interrupt_funcs[224]
per_CPU的，动态创建的硬件中断表out_interrupt_funcs[224]
这两个表项的内容为0时认为空闲，为非0时认为有效
而all_exception_interrupt_entry_lv1的参数定义为
void all_exception_interrupt_entry_lv1(x64_standard_context*ctx,uint8_t vec);
而void all_exception_interrupt_entry_lv1(x64_standard_context*ctx,uint8_t vec);的进一步逻辑为
void all_exception_interrupt_entry_lv1(x64_standard_context*ctx,uint8_t vec){
    //先根据ctx是否是被中断自用户态而进行GS_SWAP，
    processor_id=GS_SLOT[PROCESSOR_ID_GS_INDEX];//处理器标识符
    //先尝试调用软中断函数表，为0跳过
    //再调用per_CPU硬件中断表
    //谁都没有匹配上则报告虚假中断，bsp_kout记录日志
}

中断函数指针表中的函数指针格式
void (interrupt_handler*)(x64_standard_context*ctx,uint8_t vec,uint32_t processor_id);这样子就可以精确定位描一个中断事件
软中断函数指针表则是
void (soft_interrupt_handler*)(x64_standard_context*ctx,uint8_t vec);
这么设计是因为软中断是同步的指令流跳转，讨论per_cpu信息没有什么意义