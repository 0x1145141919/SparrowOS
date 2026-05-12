此文档为init.elf的page_allocator数据结构组织+接口功能规范文档
此分配器是基于page*mem_map的页框数组与phyinterval_t* mem_map_intervals的区间描述符进行各物理页框的信息的数据结构进行可分配内存页框的分配与管理
下面是核心数据结构定义：
static page*     mem_map;  为页框数组，在init函数中根据basic_allocator::get_pure_memory_view的物理区间表，根据所有freeSystemRam的页框的数目分配连续物理地址空间，使用basic_allocator::pages_alloc分配算法以及basic_allocator::pages_set
phyinterval_t* mem_map_intervals; 描述物理页框区间的描述符数组，
首先是字段语义介绍
struct phyinterval_t {
        uint64_t base; //区间起始物理地址
        uint64_t numof4kbpgs; //区间内页框数目
        uint64_t baseidx_in_memmap; //区间起始页框在mem_map中的索引
    };
mem_map_intervals的描述符数组遵守base，baseidx_in_memmap随着描述符数组单调递增而单调递增的顺序，根据basic_allocator::get_pure_memory_view里面的所有freeSystemRam区间转生而来

辅助字段：
static uint64_t  mem_map_page_count;   // mem_map 覆盖的页框总数
static phyaddr_t mem_map_pbase;        // mem_map 的物理基址
static uint64_t  mem_map_bytes;        // mem_map 占用字节数
static uint64_t  mem_map_intervals_count; // mem_map_intervals 数组元素个数
static uint64_t  free_pages;           // 当前空闲页框数（实时更新）

scan_top_base 是瞬态端的向下扫描光标，初始化值为 mem_map_intervals 最后一个区间的地址上限，每次 allocate 后推进至分配到的基址（递减）
scan_down_base 是保持端的向上扫描光标，初始化值为 max(1MB, mem_map_intervals[0].base)，每次 allocate 后推进至分配的末尾（递增）

由是进行接口定义：
init()进行初始化
    内部的工作内容就是根据basic_allocator::get_pure_memory_view初始化mem_map，mem_map_intervals，scan_top_base，scan_down_base这些核心数据结构以及辅助字段。其中phyinterval_t* mem_map_intervals的数组是new从堆上分配
    mem_map初始化好之后要使用pages_set对mem_map数组以reserved类型自标记，对init.elf映像本身pages_set用{
        (uint64_t)&__init_text_start为内核镜像开始
        (uint64_t)&__init_heap_end为内核镜像结尾，
        进行init.elf本体的自标记
    }
    
phyaddr_t available_meminterval_probe_keep(uint64_t page_count, uint8_t align_log2 = 12)从scan_down_base的区间的地址，向上，逐区间使用interval_bottom_to_top_ff_scan首次适配算法查询足量空闲的物理地址区间，找到后推进scan_down_base光标至分配区间末尾
phyaddr_t available_meminterval_probe(uint64_t page_count, uint8_t align_log2 = 12)从scan_top_base的区间的地址，向下，逐区间使用interval_top_to_bottom_ff_scan首次适配算法查询有足量空闲的物理地址区，找到后推进scan_top_base光标至分配到的基址

void relinquish_mem_map(phyaddr_t* out_pbase, uint64_t* out_pcount)生命周期终止的自裁接口，在内部mem_map完全清零后，通过指针返回mem_map物理地址长度以及总计页框数目

int page_allocator::pages_set(mem_interval interval, page_state_t state)从地址区间找到完全匹配的mem_map_intervals，并在mem_map_intervals的对应页框中进行设置，若没有落到完整区间子集则报错


查询接口：
uint64_t free_page_count()      返回当前空闲页框数
uint64_t total_page_count()     返回 mem_map 覆盖的总页框数
const page* get_mem_map()       返回 mem_map 指针
phyaddr_t get_mem_map_pbase()   返回 mem_map 物理基址

不过由于区间描述符数组管理mem_map数组的需求，我认为要至少实现如下内部接口：
phyinterval_t*get_interval_by_addr(addr_t addr)内部通过线性扫描直接从物理地址找到对应的区间描述符，返回指针
int interval_set(mem_interval interval, page_state_t state)内部通过get_interval_by_addr()找到对应的区间描述符，并设置对应页框的状态；pages_set 的实质实现体
phyaddr_t interval_top_to_bottom_ff_scan(phyinterval_t* interval,uint64_t page_count,uint8_t align_log2);
区间内从高地址向低地址的首次适应线性扫描。
返回被找到区间的物理基址（phyaddr_t，0=失败）。
保证返回的基址符合 align_log2 对齐，且从基址向上连续 page_count 个页框均为空闲页。
phyaddr_t interval_bottom_to_top_ff_scan(phyinterval_t* interval,uint64_t page_count,uint8_t align_log2);
区间内从低地址向高地址的首次适应线性扫描。
与 interval_top_to_bottom_ff_scan 相同的返回保证。
