#pragma once
#include "stdint.h"
#include "Memory.h"
typedef  uint64_t phyaddr_t;
typedef uint8_t bitset512_t[64];
typedef uint8_t pgsbitmap_entry2bits_width[128];

enum cache_strategy_t:uint8_t
{
    UC=0,
    WC=1,
    WT=4,
    WP=5,
    WB=6,
    UC_minus=7
};
union ia32_pat_t
{
   uint64_t value;
   cache_strategy_t  mapped_entry[8];
};
struct pgaccess
{
    uint8_t is_kernel:1;
    uint8_t is_writeable:1;
    uint8_t is_readable:1;
    uint8_t is_executable:1;
    uint8_t is_global:1;
    uint8_t is_occupyied:1;
    cache_strategy_t cache_strategy;
};
#define PHY_ATOM_PAGE 0
#define VIR_ATOM_PAGE 1
enum short_pg_type:uint8_t{
    FREE=0,
    OCCUPYIED=1,
    RESERVED=2,
    NOT_EXIST=3
};
struct minimal_phymem_seg_t
{
    phyaddr_t base;
    uint64_t num_of_4kbpgs:50;
    short_pg_type type:2;
    uint16_t remapped_count;
};
struct pgflags
{
    uint64_t physical_or_virtual_pg:1;//0表示物理页，1表示虚拟页
    uint64_t is_exist:1;//物理页是否存在，虚拟页若是不存在要考虑在交换空间
    uint64_t is_atom:1;//原子节点才能被锁，内核标识位，读写运行权限位有效
   uint64_t is_reserved:1;
   uint64_t is_occupied:1;
    /**
     * 在physical_or_virtual_pg为物理表，存在，是原子节点的情况下
     * 标记这个表是不是空闲表还是非空闲表
     */
    uint64_t is_kernel:1;
    //低物理地址恒等映射内存默认且必须全部设为内核内存
    uint64_t is_readable:1;
    uint64_t is_writable:1;
    uint64_t is_executable:1;
    uint64_t is_remaped:1;
    //有些物理页必然会被重映射，无论内核还是用户态
    uint64_t pg_lv:3;
    cache_strategy_t cache_strateggy:3;
    uint64_t is_global:1;
    
};
struct psmemmgr_flags_t
{   
    uint64_t is_new_kspace_cr3_valid:1;
    uint64_t is_PCID_enable:1;
    
};
struct lowerlv_PgCBtb;
struct lowerlv_bitmap_entry_width1bit;
struct lowerlv_bitmap_entry_width2bits;

struct  PgControlBlockHeader
{
    pgflags flags;
    union 
    {
        lowerlv_PgCBtb* lowerlvPgCBtb;
        phyaddr_t base_phyaddr;
    }base;     
};
constexpr PgControlBlockHeader NullPgControlBlockHeader={0};
struct lowerlv_PgCBtb
{
    uint8_t* pgextention;//父表项的拓展数据
    PgControlBlockHeader entries[512];
};
struct vaddr_seg_subtb_t
{
    phyaddr_t phybase;
    uint64_t num_of_4kbpgs;
};
typedef struct PgControlBlockHeader PgCBlv4header;
typedef struct PgControlBlockHeader PgCBlv3header;
typedef struct PgControlBlockHeader PgCBlv2header;
typedef struct PgControlBlockHeader PgCBlv1header;
typedef struct PgControlBlockHeader PgCBlv0header;

class KernelSpacePgsMemMgr
{
private:
struct pgs_queue_entry_t
{
    uint64_t pgs_count;
    uint8_t pgs_lv;
    uint8_t reserved;
};


struct phymem_pgs_queue//这是在物理内存描述符表转换为类页表结构中的中间体数据结构，用数组存储存储这段内存需要的页表
/**
 * 同理，只要是基址+段长都可以转换成这个数据结构
 */
{
    uint8_t entry_count=0;
    pgs_queue_entry_t entry[10]={0};//最极端的情况是4kb,2mb,1gb,512gb,256tb,512gb,1gb,2mb,4kb
};
ia32_pat_t cache_strategy_table;
uint8_t cpu_pglv;//存着cpu处于几级分页模式/四级还是五级
//四级分页最大支持128TB内存，五级分页最大支持64PB内存，留一半映射到高位内存空间作为高位内核空间
uint16_t kernel_sapce_PCID;
class pgtb_heap_mgr_t{
    
    static const uint8_t num_of_2mbpgs=2;
    bitset512_t maps[num_of_2mbpgs];
    public:
    phyaddr_t heap_base;
    uint64_t* pgtb_root_phyaddr;
    uint16_t root_index;
    pgtb_heap_mgr_t();
    void all_entries_clear();//清空所有位图中的位,代表释放堆里面所有的页
    //只有创建页表项失败的时候才会调用这个函数
    void*pgalloc();
    void free(phyaddr_t addr);
    void clear(phyaddr_t addr);
};
pgtb_heap_mgr_t*pgtb_heap_ptr;
psmemmgr_flags_t flags;

PgControlBlockHeader*rootlv4PgCBtb=nullptr;
// 辅助函数：打印四级表
void PrintLevel4Table(lowerlv_PgCBtb* table, int parentIndex);
// 辅助函数：打印三级表
void PrintLevel3Table(lowerlv_PgCBtb* table, int grandParentIndex, int parentIndex);
// 辅助函数：打印二级表
void PrintLevel2Table(lowerlv_PgCBtb* table, int greatGrandParentIndex, int grandParentIndex, int parentIndex);
// 辅助函数：打印一级表
void PrintLevel1Table(lowerlv_PgCBtb* table, int greatGreatGrandParentIndex, int greatGrandParentIndex, int grandParentIndex, int parentIndex);
// 辅助函数：计算物理地址
uint64_t CalculatePhysicalAddress(int index5, int index4, int index3, int index2, int index1, int index0);
// 辅助函数：打印页表项信息
void PrintPageTableEntry(PgControlBlockHeader* entry, int level);
int pgtb_entry_convert(uint64_t&pgtb_entry,
    PgControlBlockHeader PgCB,
    phyaddr_t alloced_loweraddr );
int process_pte_level(lowerlv_PgCBtb *pt_PgCBtb, uint64_t *pt_base, uint64_t base_vaddr);
int process_pd_level(lowerlv_PgCBtb *pd_PgCBtb, uint64_t *pd_base, uint64_t base_vaddr, bool is_5level);
/**
 * 根据物理地址通过PML5_INDEX_MASK_lv4提取引索
 * 而后根据其原有项是否存在再说是否为其创建子表，
 * 下面还有几个级别的也是如此
 */
int PgCBtb_lv4_entry_construct(uint64_t addr,pgflags flags,phyaddr_t mapped_phyaddr=0);
int PgCBtb_lv3_entry_construct(uint64_t addr,pgflags flags,phyaddr_t mapped_phyaddr=0);
int PgCBtb_lv2_entry_construct(uint64_t addr,pgflags flags,phyaddr_t mapped_phyaddr=0);
int PgCBtb_lv1_entry_construct(uint64_t addr,pgflags flags,phyaddr_t mapped_phyaddr=0);
int PgCBtb_lv0_entry_construct(uint64_t addr,pgflags flags,phyaddr_t mapped_phyaddr=0);
int (KernelSpacePgsMemMgr::*PgCBtb_construct_func[5])(uint64_t,pgflags,phyaddr_t)=
{
    &KernelSpacePgsMemMgr::PgCBtb_lv0_entry_construct,
    &KernelSpacePgsMemMgr::PgCBtb_lv1_entry_construct,
    &KernelSpacePgsMemMgr::PgCBtb_lv2_entry_construct,
    &KernelSpacePgsMemMgr::PgCBtb_lv3_entry_construct,
    &KernelSpacePgsMemMgr::PgCBtb_lv4_entry_construct
};
int construct_pgsbasedon_phy_memDescriptor (phy_memDescriptor memDescriptor);
/**
 * 第0级别表在创建的时候会对高级别的表项进行解析，初始化，
 * 如果存在高级别表项那么就直接使用，不存在那么就创建
 * 比如在完全没有高级别表项的时候，0地址rootlv4PgCBtb一个节点一个节点的开始构建
 * 这几个函数会直接复制flags的表项，不做任何检查
 */
/**
 * 其它级别表项的创建方式与第0级别表项类似，只是解析的表项不同
 */
/**
 * 下面是五个查询函数，根据物理地址返回对应的表项
 */
PgControlBlockHeader&PgCBtb_lv4_entry_query(phyaddr_t addr);
PgControlBlockHeader&PgCBtb_lv3_entry_query(phyaddr_t addr);
PgControlBlockHeader&PgCBtb_lv2_entry_query(phyaddr_t addr);
PgControlBlockHeader&PgCBtb_lv1_entry_query(phyaddr_t addr);
PgControlBlockHeader&PgCBtb_lv0_entry_query(phyaddr_t addr);
PgControlBlockHeader&(KernelSpacePgsMemMgr::*PgCBtb_query_func[5])(phyaddr_t)=
{
    &KernelSpacePgsMemMgr::PgCBtb_lv0_entry_query,
    &KernelSpacePgsMemMgr::PgCBtb_lv1_entry_query,
    &KernelSpacePgsMemMgr::PgCBtb_lv2_entry_query,
    &KernelSpacePgsMemMgr::PgCBtb_lv3_entry_query,
    &KernelSpacePgsMemMgr::PgCBtb_lv4_entry_query
};

// 定义函数指针类型
/**
 * 此函数用于把一段内存转换成一个队列，方便后续处理，用完这个队列记得手动释放
 */

phymem_pgs_queue*seg_to_queue(phyaddr_t base,uint64_t size_in_bytes);
int construct_pde_level(uint64_t *pd_base, lowerlv_PgCBtb *pd_PgCBtb, uint64_t pml4_index, uint64_t pdpt_index);
int construct_pdpte_level(uint64_t *pdpt_base, lowerlv_PgCBtb *pdpt_PgCBtb, uint64_t pml4_index);
int construct_pml4e_level(uint64_t *rootPgtb, lowerlv_PgCBtb *root_PgCBtb);
int process_pdpt_level(lowerlv_PgCBtb *pdpt_PgCBtb, uint64_t *pdpt_base, uint64_t base_vaddr, bool is_5level);
int process_pml4_level(lowerlv_PgCBtb *pml4_PgCBtb, uint64_t *pml4_base, uint64_t base_vaddr, bool is_5level);
int process_pml5_level(PgControlBlockHeader *pml5_PgCBtb, uint64_t *pml5_base, bool is_5level);
int pgtb_lowaddr_equalmap_construct_4lvpg();
int pgtb_lowaddr_equalmap_construct_5lvpg();
/**
 * 为了动态虚拟内存管理需要定义的数据结构有
 * 1.虚拟内存对象表数组
 * 2.可分配物理内存段表数组
 */




struct allocatable_mem_seg_t
{
    phyaddr_t base;
    uint64_t size_in_numof4kbpgs;
    uint64_t max_num_of_subtb_entries;
    uint64_t num_of_subtb_entries;
    minimal_phymem_seg_t*subtb;//子表里面只会存放占用的物理内存段
    //没有被描述到的物理内存段就是没有占用
};
/*这个嵌套类是物理内存占用管理的子系统
专门管理各个可分配内存段的物理内存分配
*/
class phymemSegsSubMgr_t{
    public:
      static constexpr uint8_t max_entry_count=64;
/*
这个子类的构造函数只会在KernelSpacePgsMemMgr的Init函数中运行，
也就是说在内存子系统初始化的时候只会运行一次
原理是扫描rootPhyMemDscptTbBsPtr表按照物理基地址从小到大提取空闲内存段，
按照引索从小到大转换成allocatable_mem_seg_t的数据结构
*/

      phymemSegsSubMgr_t();
      void Init();
/*
查询是否有对应基址的占用内存段并在重映射计数上加一
自然不能超过0xffff
*/
int remap_inc(phyaddr_t base);
/*
查询是否有对应基址的占用内存段并在重映射计数上减一
自然不能小于0
*/
    int remap_dec(phyaddr_t base);
    /*
根据可分配内存段表使用分段查找技术查询有没有空闲内存，顺序为低地址向高地址扫描
*/
    
    void *alloc(uint64_t num_of_4kbpgs, uint8_t align_require = 12);
    bool is_addrrange_have_intersect(IN phyaddr_t base, IN size_t numof_4kbpgs, IN minimal_phymem_seg_t subtb);
    /*
        固定地址分配函数，这个函数会先检测固定地址的物理内存段是否存在被占用段，
        根据传入的物理地址和长度进行物理内存分配，会在allocatable_mem_seg
        某一项的子表之下生成一个表项，子表项保持物理基址随引索增大而增大的顺序
        */
    int fixedaddr_allocate(IN phyaddr_t base, IN size_t num_of_4kbpgs);
/**
 * 碎片分配函数，从低地址0x100000开始扫描，分配每一个内存碎片，如果最后分配到了足够的空间就交返回数组地址
 * 反之返回空指针
 */
 /*
根据物理基址查询相应的表项，由于不存在合并，拆分，直接把相应子表项使用线性表删除即可
*/   int free(phyaddr_t base);
 vaddr_seg_subtb_t *fragments_alloc(uint64_t numof_4kbpgs, OUT uint16_t &allocated_count);
/**
 * 查询物理内存使用情况
*查询allocatable_mem_seg[max_entry_count]以及每个表项的子表，allocatable_mem_seg映射到的内存若被子表项映射到则为OCCUPYIED
反之则为FREE,没有被allocatable_mem_seg映射到的为RESERVED,Not_exsit是通过gBaseMemMgr获得的高于最大物理内存空间的内存
尽可能融合相邻占用表项
 */
 minimal_phymem_seg_t*memsegs_usage_query(phyaddr_t base,uint64_t offset,uint64_t&result_entry_count);
 /*与上一个函数类似，但是只查询
 一个地址的使用情况
 */
  minimal_phymem_seg_t addr_query(phyaddr_t phyaddr);
 private:
 allocatable_mem_seg_t allocatable_mem_seg[max_entry_count];
 uint8_t allocatable_mem_seg_count;
};
phymemSegsSubMgr_t phymemSubMgr;
struct vaddr_seg_t
{
    vaddr_t base;
    pgflags flags;
    uint64_t size_in_numof4kbpgs;
    uint16_t max_num_of_subtb_entries;
    uint16_t num_of_subtb_entries;
    PHY_MEM_TYPE type;
    vaddr_seg_subtb_t*subtb;
};

uint16_t vaddrobj_count=0;//占用了物理内存的虚拟内存对象数量(不包括空闲的)
/**
 *vaddr_objs数组存放的是虚拟内存对象，
 表项按照虚拟基址增大的顺序存放排列，不保存空闲虚拟内存对象，可能有空隙但不可能存在重复空间
 */
vaddr_seg_t vaddr_objs[4096];
void enable_new_cr3();
int construct_pte_level(uint64_t *pt_base, lowerlv_PgCBtb *pt_PgCBtb, uint64_t pml4_index, uint64_t pdpt_index, uint64_t pd_index);
phyaddr_t Inner_fixed_addr_manage(phyaddr_t linear_base,
                                  phymem_pgs_queue queue,
                                  pgaccess access,
                                  phyaddr_t mapped_phybase=0,
                                  bool modify_pgtb = false);
int process_pte_level(uint64_t *pt_base, lowerlv_PgCBtb *pt_PgCBtb, uint64_t &scan_addr, uint64_t endaddr, uint64_t pml4_index, uint64_t pdpt_index, uint64_t pd_index, uint64_t start_pt_index);
int process_pde_level(uint64_t *pd_base, lowerlv_PgCBtb *pd_PgCBtb, uint64_t &scan_addr, uint64_t endaddr, uint64_t pml4_index, uint64_t pdpt_index, uint64_t start_pd_index);
int process_pdpte_level(uint64_t *pdpt_base, lowerlv_PgCBtb *pdpt_PgCBtb, uint64_t &scan_addr, uint64_t endaddr, uint64_t pml4_index, uint64_t start_pdpt_index);
int process_pml4e_level(uint64_t *rootPgtb, lowerlv_PgCBtb *root_PgCBtb, uint64_t &scan_addr, uint64_t endaddr, uint64_t start_pml4_index);
int process_pml5e_level(uint64_t *rootPgtb, lowerlv_PgCBtb *root_PgCBtb, uint64_t &scan_addr, uint64_t endaddr, uint64_t start_pml5_index);
int modify_pgtb_in_4lv(uint64_t base, uint64_t endaddr);
int modify_pgtb_in_5lv(phyaddr_t base,uint64_t endaddr);


public:
const pgaccess PG_RW={1,1,1,0,1,1,WB};
const pgaccess PG_RWX ={1,1,1,1,1,1,WB};
const pgaccess PG_R ={1,0,1,0,1,0,WB};

void *pgs_allocate(uint64_t size_in_byte, pgaccess access, uint8_t align_require);
int pgs_fixedaddr_allocate(IN phyaddr_t addr, IN size_t size_in_byte, pgaccess access);
int pgs_free(phyaddr_t addr, size_t size_in_byte);
/**
 * 根据权限要求以及对齐要求，大小要求先phymemSubMgr子系统中分配一片连续的虚拟地址空间（高一半内核空间）
 * （优先分配连续物理内存，连续失败就走phymemSubMgr的碎片分配函数）
 * 由于表项按照虚拟基址增大的顺序存放排列，不保存空闲虚拟内存对象，可能有空隙但不可能存在重复空间，
 * 要从前向后扫描一个空闲虚拟地址空间
 * 以及最后使用Inner_fixed_addr_manage分配
*/
void*pgs_allocate_remapped(size_t size_in_byte, pgflags flags,uint8_t align_require=12);
/**
 * 确定物理地址尝试分配一个连续的物理内存空间返回一个分配的连续的虚拟地址空间
 * 根据权限要求，物理地址基址，大小要求先phymemSubMgr子系统中使用固定物理地址分配器尝试分配物理内存，再分配一片连续的虚拟地址空间（高一半内核空间）
 * 由于表项按照虚拟基址增大的顺序存放排列，不保存空闲虚拟内存对象，可能有空隙但不可能存在重复空间，
 *  要从前向后扫描一个空闲虚拟地址空间
 * 以及最后使用Inner_fixed_addr_manage分配
 * 同一个物理地址空间不可以被重复分配，但可以被多次重映射
 */
void* pgs_fixedaddr_allocate_remapped(IN phyaddr_t addr, IN size_t size_in_byte,IN pgaccess access);
/**
 * 先释放虚拟地址空间，再尝试释放物理地址空间
 * 只有指向的物理段的引用数降为0时才释放物理地址空间
 * 虚拟地址空间的删除使用线性表删除函数
 */
int pgs_remapped_free(vaddr_t addr);
/**
 * 根据物理基址addr使用phymemSubMgr子系统中物理内存段增减引用数
 *若是失败再尝试使用gBaseMemMgr的物理内存段增减引用数接口
 *成功增加引用数之后用对应的查询接口得到相应物理内存段的
*如果vbase参数非0先检查是不是有效的虚拟地址（4/5级分页下的高一般线性地址以及4kb对齐是否满足）
 *vbase为0则自动分配 
 *然后就是在vaddr_objs里面新增虚拟内存对象（参照pgs_allocate_remapped扫描一个空闲虚拟地址空间）以及调用Inner_fixed_addr_manage
 
 */
void*pgs_remapp(phyaddr_t addr,pgflags flags,vaddr_t vbase=0);
void *phy_pgs_allocate(uint64_t size_in_byte, uint8_t align_require);//此接口分配的内存基于物理地址，不支持权限配置，只能配置为读写权限
int fixedaddr_phy_pgs_allocate(phyaddr_t addr,uint64_t size_in_byte);
int free_phy_pgs(phyaddr_t addr);
    phy_memDescriptor* queryPhysicalMemoryUsage(phyaddr_t base,uint64_t len_in_bytes);
    phy_memDescriptor* getPhyMemoryspace();
    /**
     * 上面四个页级别物理内存分配器必须在is_pgsallocate_enable开启后才能使用    
     */
static constexpr pgflags kspace_data_flags = {
    .physical_or_virtual_pg = VIR_ATOM_PAGE,
    .is_exist = 1,
    .is_atom = 1,
    .is_reserved = 0,
    .is_occupied = 1,
    .is_kernel = 1,
    .is_readable = 1,
    .is_writable = 1,
    .is_executable = 0,
    .is_remaped = 1,
    .pg_lv = 0,
    .cache_strateggy = WB,
    .is_global = 1
};
static constexpr pgflags kspace_mmio_flags = {
    .physical_or_virtual_pg = VIR_ATOM_PAGE,
    .is_exist = 1,
    .is_atom = 1,
    .is_reserved = 1,
    .is_occupied = 1,
    .is_kernel = 1,
    .is_readable = 1,
    .is_writable = 1,
    .is_executable = 0,
    .is_remaped = 1,
    .pg_lv = 0,
    .cache_strateggy = UC,
    .is_global = 1
};
static constexpr pgflags kspace_code_flags = {
    .physical_or_virtual_pg = VIR_ATOM_PAGE,
    .is_exist = 1,
    .is_atom = 1,
    .is_reserved = 0,
    .is_occupied = 1,
    .is_kernel = 1,
    .is_readable = 1,
    .is_writable = 0,
    .is_executable = 1,
    .is_remaped = 1,
    .pg_lv = 0,
    .cache_strateggy = WB,
    .is_global = 1
};
    void Init();
    void PrintPgsMemMgrStructure();
    /*
    根据设计，一个虚拟地址只会映射到一个物理地址，但是一个物理地址可能映射到多个虚拟地址
    这个地址转换函数负责虚拟地址转换，原理是查询vaddr_objs[4096]以及其子表的数据结构
    */
    void* v_to_phyaddrtraslation(vaddr_t vaddr);
};
extern KernelSpacePgsMemMgr gKspacePgsMemMgr;
void print_PgControlBlockHeader(struct PgControlBlockHeader* header);