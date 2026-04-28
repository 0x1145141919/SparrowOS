kshell 内存操作命令设计规范
1. 命令总览
命令	功能	风险等级	优先级
palloc	物理页分配	WARNING	P0
pfree	物理页释放	DANGEROUS	P0
valloc	匿名虚拟页分配	WARNING	P0
vfree	匿名虚拟页释放	DANGEROUS	P0
pread	直接物理内存读取	SAFE	P1
pwrite	直接物理内存写入	DANGEROUS	P1
pmap	物理→虚拟映射	WARNING	P1
punmap	解除虚拟映射	DANGEROUS	P1
dmap	直接物理映射（用户空间兼容）	WARNING	P2
stackalloc	内核栈分配	SAFE	P2
2. 安全约束
风险等级	确认机制
SAFE	无需确认，直接执行
WARNING	打印警告，要求输入 y 确认
DANGEROUS	显示醒目警告，要求输入 YES 确认，支持 -f 跳过

    所有分配/释放命令必须维护分配历史记录（静态数组，容量 256），用于防止重复释放和参数校验

    释放时必须完全匹配分配的地址和大小，否则拒绝执行

3. Phase 0 命令
palloc — 物理页分配

    参数：<size_bytes> [align_log2] [type]

    约束：size_bytes 必须是 4KB 倍数，align_log2 范围 12–30

    输出：物理地址、Order、BCB ID

    不自动映射到虚拟地址空间

pfree — 物理页释放

    参数：<phyaddr> <size_bytes> [-f]

    校验：地址和大小必须与分配历史匹配

    输出：释放成功信息

valloc — 匿名虚拟页分配

    参数：<pages_count> [alignment_log2] [type]

    输出：虚拟地址、对应的物理地址、映射权限（默认 RW, WB）

    内部自动完成：物理分配 + 虚拟映射

vfree — 匿名虚拟页释放

    参数：<vaddr> <pages_count> [-f]

    校验：地址和页数必须与分配历史匹配

    输出：释放成功信息

4. Phase 1 命令
pread — 直接物理内存读取

    参数：<phyaddr> <size_bytes> [format]

    size 支持：1/2/4/8 字节

    format：hex（默认）/ dec / ascii

    输出：格式化后的值

pwrite — 直接物理内存写入

    参数：<phyaddr> <value> [size] [-f]

    size 默认 4 字节，支持 1/2/4/8

    必须要求输入 YES 确认（-f 可跳过，但需显示警告）

    输出：写入成功信息

pmap — 物理→虚拟映射

    参数：<phyaddr> <vaddr> <size_bytes> [access]

    vaddr 可为 0 表示自动分配

    access 默认 RW，可选 R、RX、RWX，后缀 :UC / :WC 控制缓存策略

    输出：映射成功信息，自动记录到映射历史

punmap — 解除虚拟映射

    参数：<vaddr> <size_bytes> [-f]

    只解除映射，不释放物理内存

    输出：解除后的虚拟地址访问提示

5. Phase 2 命令
dmap — 直接物理映射

    参数：<phyaddr> <size_bytes>

    映射到固定区域，用户空间可见

    输出：映射的虚拟地址

stackalloc — 内核栈分配

    参数：<pages_count>

    栈从高地址向低增长，栈底下方自动分配 1 页缓冲（用于溢出检测）

    输出：栈底（高地址）、栈顶（低地址）、缓冲区域