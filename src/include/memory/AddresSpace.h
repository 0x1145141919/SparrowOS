#pragma once
#include "stdint.h"
#include "memory/memory_base.h"
#include "abi/arch/x86-64/pgtable45.h"
#include <util/lock.h>
#include "util/Ktemplats.h"
#include "memmodule_err_definitions.h"
namespace PAGE_TBALE_LV{
    constexpr bool LV_4=true;
    constexpr bool LV_5=false;
}
constexpr uint16_t KERNEL_SPACE_PCID=0;
extern bool pglv_4_or_5;//true代表4级页表，false代表5级页表,在KspacMapMgr.cpp存在

int VM_vaddr_cmp(VM_DESC* a,VM_DESC* b);
namespace MEMMODULE_LOCAIONS{
    namespace ADDRESSPACE_EVENTS{
        constexpr uint8_t EVENT_CODE_INIT=0x0;
        constexpr uint8_t EVENT_CODE_ENABLE_VMENTRY=0x1;
        constexpr uint8_t EVENT_CODE_DISABLE_VMENTRY=0x2;
        constexpr uint8_t EVENT_CODE_TRAN_TO_PHY=0x3;
        constexpr uint8_t EVENT_CODE_INVALIDATE_TLB=0x4;
        constexpr uint8_t EVENT_CODE_BUILD_INDENTITY_MAP_ONLY_ON_gKERNELSPACE=0x5;
        constexpr uint8_t EVENT_CODE_UNREGIST=0xff;
        namespace ENABLE_VMENTRY_RESULTS{
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_VMENTRY_congruence_vlidation=0x1;
                constexpr uint16_t REASON_CODE_BAD_VMENTRY=0x3;
                constexpr uint16_t REASON_CODE_BAD_VMENTRY_CANT_SPLIT=0x4;
                constexpr uint16_t REASON_CODE_NOT_SUPPORT_LV5_PAGING=0x100;
                }
            namespace FATAL_REASONS{
                    constexpr uint16_t REASON_CODE_TRY_TO_GET_SUB_PAGE_IN_ATOM_PAGE=0x1;//AddressSpace的设计的能力有限，
                    //只能根据喂进来的VM_DESC指定物理地址虚拟地址建立页表结构，
                    //但是能够对已经确定的大页试图向下建立小页报错，这是种严重错误，
                    //但是诱因可能是外部调用错误，地址段覆盖，所以这个不会内部panic,
                    //但是调用者必须有意识，出现这个错误码可以考虑重开
                    constexpr uint16_t REASON_CODE_PAGES_SET_FALT=0x2;
                    constexpr uint16_t REASON_CODE_INVALID_PAGE_SIZE=0x4;
                }
            namespace SUCCESS_BUT_SIDE_AFFECTS{
                constexpr uint16_t REASON_CODE_MAP_LOW_16K=0x1;
            }            
        }
        namespace TRAN_TO_PHY_RESULTS_CODE{
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_NOT_ALLOW_KSPACE_VA=0x1;
                constexpr uint16_t REASON_CODE_NOT_PRESENT_ENTRY=0x2;  
                constexpr uint16_t REASON_CODE_NOT_SUPPORT_LV5_PAGING=0x100;   
                }
        }
        namespace DISABLE_VMENTRY_RESULTS{
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_INVALID_PAGETABLE_ENTRY=0x1;
                constexpr uint16_t REASON_CODE_BAD_VMENTRY_TRY_TO_MAP_LOW_MEM_WHO_NOT_gKernelSpace=0x2;
                constexpr uint16_t REASON_CODE_BAD_VMENTRY=0x3;
                constexpr uint16_t REASON_CODE_BAD_VMENTRY_CANT_SPLIT=0x4;
                constexpr uint16_t REASON_CODE_NOT_SUPPORT_LV5_PAGING=0x100;
                    
                }
            namespace FATAL_REASONS{
                    constexpr uint16_t REASON_CODE_TRY_TO_GET_SUB_PAGE_IN_ATOM_PAGE=0x1;//AddressSpace的设计的能力有限，
                    //只能根据喂进来的VM_DESC指定物理地址虚拟地址建立页表结构，
                    //但是能够对已经确定的大页试图向下建立小页报错，这是种严重错误，
                    //但是诱因可能是外部调用错误，地址段覆盖，所以这个不会内部panic,
                    //但是调用者必须有意识，出现这个错误码可以考虑重开
                    constexpr uint16_t REASON_CODE_PAGES_SET_FALT=0x2;
                    constexpr uint16_t REASON_CODE_INVALID_PAGE_SIZE=0x4;
                    constexpr uint16_t REASON_CODE_TRY_TO_GET_SUB_PAGE_OF_NOT_PRESENT_PAGE=0x5;
                    constexpr uint16_t REASON_CODE_TRY_TO_CLEAR_UNPRESENT_PAGE=0x6;
                    constexpr uint16_t REASON_CODE_CONSISTENCY_VIOLATION_WHEN_CLEAR_PAGE_TABLE_ENTRY=0x7;
                    constexpr uint16_t REASON_CODE_OTHER_PAGES_SET_FATAL=0x8;
                }   
        }
        namespace INVALIDATE_TLB_RESULTS{
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_BAD_VM_ENTRY=0x1; 
            }
            namespace FATAL_REASONS{ 
                constexpr uint16_t REASON_CODE_INVALID_PAGE_SIZE=0x4;
            }
        }
        namespace BUILD_INDENTITY_MAP_ONLY_ON_gKERNELSPACE{
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_NOT_gKERNELSPACE=1;
            }
        }
    }
    constexpr uint8_t LOCATION_CODE_KSPACE_MAP_MGR_VMENTRY_RBTREE=17;
    namespace KSPACE_MAPPER_EVENTS{
        constexpr uint8_t EVENT_CODE_INIT=0x0;
        namespace INIT_RESULTS{
            namespace FAIL_REASONS{
                constexpr uint16_t USER_TEST_KERNEL_IMAGE_SET_FAIL=0x1;
                constexpr uint16_t USER_TEST_KERNEL_IMAGE_BAD_ELF_MAGIC=0x2;
            }
        }
        constexpr uint8_t EVENT_CODE_ENABLE_VMENTRY=0x1;
        constexpr uint8_t EVENT_CODE_DISABLE_VMENTRY=0x2;
        constexpr uint8_t EVENT_CODE_TRAN_TO_PHY_ENTRY=0x3;
        namespace TRAN_TO_PHY_ENTRY_RESULTS_CODE{
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_NOT_PRESENT_ENTRY=0x2;  
                constexpr uint16_t REASON_CODE_NOT_SUPPORT_LV5_PAGING=0x100;   
            }
            namespace FATAL_REASONS{
                constexpr uint16_t REASON_CODE_UNREACHABLE_CODE=0x4;
            }
        }
        constexpr uint8_t EVENT_CODE_INVALIDATE_TLB=0x4;
        namespace INVALIDATE_TLB_RESULTS{
            namespace FAIL_REASONS{
                constexpr uint16_t SHARED_INFO_PACKAGE_NOT_INITILAIZED=0x1; 
            }
        }
        constexpr uint8_t EVENT_CODE_UNREGIST=0xff;
        namespace ENABLE_VMENTRY_RESULTS{
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_VMENTRY_congruence_vlidation=0x1;
                constexpr uint16_t REASON_CODE_BAD_VMENTRY=0x3;
                constexpr uint16_t REASON_CODE_NOT_SUPPORT_LV5_PAGING=0x100;
                }
            namespace FATAL_REASONS{
                    constexpr uint16_t REASON_CODE_INVALIDE_PAGES_SIZE=0x1;//AddressSpace的设计的能力有限，
                    
                }            
        }

        namespace DISABLE_VMENTRY_RESULTS{
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_BAD_VMENTRY=0x3;
                }
            namespace FATAL_REASONS{
                    constexpr uint16_t REASON_CODE_INVALIDE_PAGES_SIZE=0x1;//AddressSpace的设计的能力有限，
                    
                } 
        }
        namespace INVALIDATE_TLB_RESULTS{
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_BAD_VM_ENTRY=0x1; 
            }
            namespace FATAL_REASONS{ 
                constexpr uint16_t REASON_CODE_INVALID_PAGE_SIZE=0x4;
            }
        }
        namespace BUILD_INDENTITY_MAP_ONLY_ON_gKERNELSPACE{
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_NOT_gKERNELSPACE=1;
            }
        }
        constexpr uint8_t EVENT_CODE_PAGES_SET=0x5;
        namespace PAGES_SET_RESULTS_CODE{ 
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_BAD_COUNT=0x1;
                constexpr uint16_t REASON_CODE_COUNT_AND_BASEINDEX_OUT_OF_RANGE=0x2;
                constexpr uint16_t REASON_CODE_BASE_NOT_ALIGNED=0x3;
            }
            namespace FATAL_REASONS{
                constexpr uint16_t REASON_CODE_HUGE_PDPTE_WHEN_GET_SUB=0x1;
                constexpr uint16_t REASON_CODE_HUGE_PDE_WHEN_GET_SUB=0x2;  
            }
        }
        constexpr uint8_t EVENT_CODE_PAGES_CLEAR=0x6;
        namespace PAGES_CLEAR_RESULTS_CODE{ 
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_BAD_COUNT=0x1;
                constexpr uint16_t REASON_CODE_COUNT_AND_BASEINDEX_OUT_OF_RANGE=0x2;
                constexpr uint16_t REASON_CODE_BASE_NOT_ALIGNED=0x3;
            }
            namespace FATAL_REASONS{
                constexpr uint16_t REASON_CODE_HUGE_PDPTE_SUBTABLE_NOT_EXIST=0x1;
                constexpr uint16_t REASON_CODE_HUGE_PDE_SUBTABLE_NOT_EXIST=0x2; 
                constexpr uint16_t REASON_CODE_HUGE_PDPTE_UNTIMELY=0x3; 
                constexpr uint16_t REASON_CODE_HUGE_PDPTE_NOT_EXIST=0x5;
                constexpr uint16_t REASON_CODE_HUGE_PDE_UNTIMELY=0x4;
                constexpr uint16_t REASON_CODE_HUGE_PDE_NOT_EXIST=0x6;

            }
        }
        constexpr uint8_t EVENT_CODE_SEG_TO_INFO_PACKAGE=0x7;
        constexpr uint8_t EVENT_CODE_VM_SEARCH_BY_ADDR=0x8;
        namespace VM_SEARCH_BY_ADDR_RESULTS{ 
            namespace FAIL_REASONS{
                constexpr uint16_t REASON_CODE_NOT_FOUND=0x1;
            }
        }
    }
    constexpr uint8_t LOCATION_CODE_KSPACE_MAP_MGR_PGS_PAGE_TABLE=18;
};
struct vm_interval{
    vaddr_t vbase;
    phyaddr_t pbase;
    uint64_t size;
    pgaccess access;
};
/**
 * 此类的职责就是创建虚拟地址空间，管理虚拟地址空间，
 * 此类的职责有且仅一个功能，就是管理相应的低一般虚拟地址空间，
 * 本类接口不接受低于64k的虚拟地址空间
 * 此类的职责只有映射虚拟地址空间，不提供虚拟地址空间管理功能
 * 若使用此类请自行实现虚拟地址空间的管理
 * 最多提供一个打印实际映射表的接口
 */
class AddressSpace//到时候进程管理器可以用这个类创建，但是内核空间还是受内核空间管理器管理
{   
    private:
    phyaddr_t pml4_phybase;
    uint64_t occupyied_size;
    constexpr static uint64_t PAGE_LV4_USERSPACE_SIZE=0x00007FFFFFFFFFFF+1;
    constexpr static uint64_t PAGE_LV5_USERSPACE_SIZE=0x00FFFFFFFFFFFFFF+1;
    static constexpr uint32_t _4KB_SIZE=0x1000;
    static constexpr uint32_t _2MB_SIZE=1ULL<<21;
    static constexpr uint32_t _1GB_SIZE=1ULL<<30;
    static constexpr uint32_t ADDR_VM_BOTTOM=4*_4KB_SIZE;
    spinrwlock_cpp_t lock;
    KURD_t   default_kurd();
    KURD_t   default_success();
    KURD_t   default_fail();
    KURD_t   default_fatal();
    public:
    AddressSpace();
    KURD_t enable_VM_desc(VM_DESC desc);
    KURD_t disable_VM_desc(VM_DESC desc);
    struct tlb_invalidate_flags{
        uint64_t hardware_addresspace_id;
        uint16_t if_not_currunt_space:1;
        uint16_t if_hardware_addresspace_id_valid:1;
    };
    KURD_t invalidate_tlb_of_VM_desc(VM_DESC desc,tlb_invalidate_flags flags);
    KURD_t second_stage_init();//new完之后马上最快的速度调用此接口，并且接受返回值进行分析
    KURD_t build_identity_map_ONLY_IN_gKERNELSPACE();//uefi运行时服务依赖于这个构建的恒等映射
    uint64_t get_occupyied_size(){
        return occupyied_size;
    }
    phyaddr_t vaddr_to_paddr(vaddr_t vaddr,KURD_t&kurd);
    void unsafe_load_pml4_to_cr3(uint16_t pcid);//这个接口会直接把当前页表加载到cr3寄存器
    ~AddressSpace();//如果cr3还装载这这个页表，删除会在堆里释放根表，虽然不会马上报错但是极度危险，最好别这么干
};
extern AddressSpace*gKernelSpace;

cache_table_idx_struct_t cache_strategy_to_idx(cache_strategy_t cache_strategy);
int VM_desc_cmp(const VM_DESC& a,const VM_DESC& b);
class kpoolmemmgr_t;
class kspace_vm_table_t:Ktemplats::RBTree<VM_DESC,VM_desc_cmp>
{
public:
    using Node = Ktemplats::RBTree<VM_DESC,VM_desc_cmp>::Node;
static constexpr alloc_flags_t specify_alloc_flag={
    .is_longtime=true,
    .is_crucial_variable=true,
    .vaddraquire=true,
    .force_first_linekd_heap=true,
    .is_when_realloc_force_new_addr=false,//在realloc中强制重新分配内存，非realloc接口忽视此位但是会忠实记录进入metadata,realloc中此位不设置会优先原地调整，原地调整解决则不会修改源地址和元数据flags
    .align_log2=4
};
protected:
    Node* allocate_node(const VM_DESC& val) override
    {
        #ifdef KERNEL_MODE
        return new (specify_alloc_flag) Node(val);
        #endif
    }
    void deallocate_node(Node* node) override
    {
        delete node;
    }
private:
    vaddr_t last_alloc_end=0;  // 缓存上次分配的虚拟地址结尾，用于优化连续分配场景
    spinlock_cpp_t lock;
    public:
    void all_node_print();
    kspace_vm_table_t();
    Node* search(vaddr_t vaddr);//是否落在对应VM_desc的[VM_DESC.start,VM_DESC.end)
    int insert(VM_DESC data);
    int remove(vaddr_t vaddr);//删除VM_desc若vaddr落在其[VM_DESC.start,VM_DESC.end)
    vaddr_t alloc_available_space(uint64_t size,uint32_t target_vaddroffset);
};
extern kspace_vm_table_t*kspace_vm_table;
/**
 * 这个类的职责有且仅有一个功能，就是管理内核空间，
 * 通过kspacePML4暴露给AddressSpace::sharing_kernel_space()强制复制高一半pml4e
 * 以及类内的kspaceUPpdpt同步所有进程空间的内核结构
 * 此类全局唯一，只管理高128tb虚拟地址空间
 */
struct KspacePageTableStatisitcs
{
    uint64_t enable_vmentry_count;
    uint64_t disable_vmentry_count;
    uint64_t invalidate_tlb_count;
    uint64_t broadcast_invalidate_tlb_count;
    uint64_t tran_to_phy_entry_count;
    struct pages_set_stistics
    {
        uint64_t no_leaf_entry_set;
        union{
        struct{
            uint64_t PDPTE_HUGE_set_count;
            uint64_t PDE_HUGE_set_count;
            uint64_t PTE_set_count;
        }x86_64;
    }specific;
    } pages_set;
    struct pages_clear_stistics
    {
        uint64_t no_leaf_entry_clear;
        union{
        struct{
            uint64_t PDPTE_HUGE_set_count;
            uint64_t PDE_HUGE_set_count;
            uint64_t PTE_set_count;
        }x86_64;
    }specific;
    } pages_clear;
};
extern KspacePageTableStatisitcs*kspace_pagetable_statistics;
class KspacePageTable//使用上面的位域结构体，在初始化函数中直接用，但在后续正式外部暴露接口中对页表项必须用原子操作函数
{
    public: 
static constexpr PageTableEntryUnion high_half_template={
    .pml4={
        .present=1,
        .RWbit=1,
        .KERNELbit=0,
        .PWT=0,
        .PCD=0,
        .accessed=0,
        .reserved=0,
        .pdpte_addr=0,
        .reserved2=0,
        .EXECUTE_DENY=0
    }
};

private://后面五级页表的时候考虑选择编译
static KURD_t default_kurd();
static KURD_t default_success();
static KURD_t default_failure();
static KURD_t default_fatal();
friend AddressSpace;
static PageTableEntryUnion*kspaceUPpdpt;
static phyaddr_t kspace_uppdpt_phyaddr;
static constexpr uint64_t PAGELV4_KSPACE_BASE=0xFFFF800000000000;
static constexpr uint64_t PAGELV5_KSPACE_BASE=0xFF00000000000000;
static constexpr uint64_t PAGELV4_KSPACE_SIZE=1ULL<<(48-1);
static constexpr uint64_t PAGELV5_KSPACE_SIZE=1ULL<<(57-1);

static constexpr uint32_t _4KB_SIZE=0x1000;
static constexpr uint32_t _2MB_SIZE=1ULL<<21;
static constexpr uint32_t _1GB_SIZE=1ULL<<30;
//这个数组按照虚拟地址从小到大排序,规定虚拟地址是主键
static spinlock_cpp_t GMlock;
friend AddressSpace::AddressSpace();
static KURD_t _4lv_pdpte_1GB_entries_set(phyaddr_t phybase,vaddr_t vaddr_base,uint16_t count,pgaccess access);//这里要求的是不能跨父页表项指针边界

static KURD_t _4lv_pde_2MB_entries_set(phyaddr_t phybase,vaddr_t vaddr_base,uint16_t count,pgaccess access);//这里要求的是不能跨页目录指针边界

static KURD_t _4lv_pte_4KB_entries_set(phyaddr_t phybase,vaddr_t vaddr_base,uint16_t count,pgaccess access);//这里要求的是不能跨页目录边界

static void invalidate_seg();
/**
 * 删除对应1个pde下对应的pte项，如果检测到全部pte项被删除则回收对应的pde项
 */
static KURD_t _4lv_pte_4KB_entries_clear(vaddr_t vaddr_base,uint16_t count);//这里要求的是不能跨页目录边界

static KURD_t _4lv_pde_2MB_entries_clear(vaddr_t vaddr_base,uint16_t count);//这里要求的是不能跨页目录指针边界

static KURD_t _4lv_pdpte_1GB_entries_clear(vaddr_t vaddr_base,uint16_t count);//这里要求的是不能跨父页表项指针边界
static KURD_t v_to_phyaddrtraslation_entry(vaddr_t vaddr,PageTableEntryUnion& result,uint32_t&page_size);
public:
static KURD_t enable_VMentry(const vm_interval& interval);
//这个函数的职责是根据vmentry的内容撤销对应的页表项映射，只对对应的页表结构进行操作
//失效对应tlb项目在函数外部完成
//以及顺便使用共享信息包填充shared_inval_kspace_VMentry_info
static KURD_t disable_VMentry(const vm_interval& interval);
static constexpr pgaccess PG_RW={1,1,1,0,1,WB};
static constexpr pgaccess PG_RWX ={1,1,1,1,1,WB};
static constexpr pgaccess PG_R ={1,0,1,0,1,WB};
static KURD_t Init(loaded_VM_interval* kspace_up_layer);
static KURD_t v_to_phyaddrtraslation(vaddr_t vaddr,phyaddr_t& result);
};
extern shared_inval_VMentry_info_t shared_inval_kspace_VMentry_info;
extern spinlock_cpp_t kspace_pagetable_modify_lock;//此锁的用途在于保证“修改vm区间红黑树+修改页表结构”的原子性
