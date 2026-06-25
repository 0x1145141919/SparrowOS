#include "phygpsmemmgr.h"
#include "kpoolmemmgr.h"
#include "Memory.h"
#include "VideoDriver.h"
#include "os_error_definitions.h"
#include "OS_utils.h"
#include "pgtable45.h"

KernelSpacePgsMemMgr gKspacePgsMemMgr;

extern "C" uint8_t _pgtb_heap_lma;
/**
 * @brief 
 * 
 * 
 * @return 
 *根据全局物理内存描述符表初始化一个类页表的数据结构，
 * 以便后面转换为真正页表
 * 为了兼容五级页表在四级页表的情况下lv4级别的表是只有一项的，也只为其分配一项
*当然，先初始化页表子系统专用堆，才可以构建页表子系统，页表使用的大页需要的是2mb的大页，同步性要求高
配置为写穿透
而后根据页表专用堆子系统与类页表数据结构构建页表，使页表生效
在新的cr3生效后才能让虚拟内存管理系统，可分配物理内存子系统上线

 */
KernelSpacePgsMemMgr::pgtb_heap_mgr_t::pgtb_heap_mgr_t()
{
    
 heap_base=(phyaddr_t)&_pgtb_heap_lma;
    setmem(maps,num_of_2mbpgs*sizeof(bitset512_t),0);
    root_index=0;
}
void KernelSpacePgsMemMgr::pgtb_heap_mgr_t::all_entries_clear()
{
    setmem(maps,num_of_2mbpgs*sizeof(bitset512_t),0);
}
void *KernelSpacePgsMemMgr::pgtb_heap_mgr_t::pgalloc()
{
    for(int i=0;i<num_of_2mbpgs;i++)
    {
        for(int j=0;j<512;j++)
        {
            bool var= getbit_entry1bit_width(&maps[i],j);
            if(var==false){
                setbit_entry1bit_width(maps+i,true,j);
                return (void*)(heap_base+(i<<21)+(j<<12));
            }
        }
    }
    return nullptr;
}
void KernelSpacePgsMemMgr::pgtb_heap_mgr_t::free(phyaddr_t addr)
{
    if(addr&PAGE_OFFSET_MASK[0]){
        kputsSecure("KernelSpacePgsMemMgr::pgtb_heap_mgr_t::free:not aligned addr");
    }
    uint64_t index=(addr-heap_base)>>12;
    if((index>>9)>255)kputsSecure("KernelSpacePgsMemMgr::pgtb_heap_mgr_t::free:out_of_range");
    setbit_entry1bit_width(&maps[index>>9],false,index&511ULL);
}

void KernelSpacePgsMemMgr::pgtb_heap_mgr_t::clear(phyaddr_t addr)
{
    setmem((void*)addr,0x1000,0);
}

void KernelSpacePgsMemMgr::Init()
{
    uint64_t cr4_tmp;
    PgCBtb_query_func[0] = &KernelSpacePgsMemMgr::PgCBtb_lv0_entry_query;
PgCBtb_query_func[1] = &KernelSpacePgsMemMgr::PgCBtb_lv1_entry_query;
PgCBtb_query_func[2] = &KernelSpacePgsMemMgr::PgCBtb_lv2_entry_query;
PgCBtb_query_func[3] = &KernelSpacePgsMemMgr::PgCBtb_lv3_entry_query;
PgCBtb_query_func[4] = &KernelSpacePgsMemMgr::PgCBtb_lv4_entry_query;
PgCBtb_construct_func[0] = &KernelSpacePgsMemMgr::PgCBtb_lv0_entry_construct;
PgCBtb_construct_func[1] = &KernelSpacePgsMemMgr::PgCBtb_lv1_entry_construct;
PgCBtb_construct_func[2] = &KernelSpacePgsMemMgr::PgCBtb_lv2_entry_construct;
PgCBtb_construct_func[3] = &KernelSpacePgsMemMgr::PgCBtb_lv3_entry_construct;
PgCBtb_construct_func[4] = &KernelSpacePgsMemMgr::PgCBtb_lv4_entry_construct;
    int status=0;
    phymemSubMgr.Init();
#ifdef     KERNEL_MODE
    asm volatile("mov %%cr4,%0" : "=r"(cr4_tmp));

     cpu_pglv=(cr4_tmp&(1ULL<<12))?5:4;
#endif
#ifdef TEST_MODE
    cpu_pglv=4;
#endif  
     if(cpu_pglv==5)
     rootlv4PgCBtb=new PgControlBlockHeader[512];
     else rootlv4PgCBtb=new PgControlBlockHeader;
     pgtb_heap_ptr=new pgtb_heap_mgr_t;
     phy_memDescriptor* phy_memDesTb=gBaseMemMgr.getGlobalPhysicalMemoryInfo();
     uint64_t entryCount=gBaseMemMgr.getRootPhysicalMemoryDescriptorTableEntryCount();
    cache_strategy_table.value=0x407050600070106;
    kernel_sapce_PCID=0;
     for(int i=0;i<entryCount;i++)
     {
        status=construct_pgsbasedon_phy_memDescriptor(phy_memDesTb[i]);
        if (status!=OS_SUCCESS)
        {
            kputsSecure("construct_pgsbasedon_phy_memDescriptor failed");
            return ;
        }   
     }
     enable_new_cr3();
     pgflags p;
     p.is_global=1;
     p.is_kernel=1;
     p.is_readable=1;
    p.is_remaped=1;
    p.is_reserved=0;
    p.physical_or_virtual_pg=VIR_ATOM_PAGE;
    p.is_exist=1;
    p.is_atom=1;
      for(int i=0;i<entryCount;i++)
     {
        switch (phy_memDesTb[i].Type)
        {
        case EFI_ACPI_RECLAIM_MEMORY:
        case EFI_ACPI_MEMORY_NVS:
        p.cache_strateggy=cache_strategy_t::WB;
        p.is_writable=1;
        p.is_executable=0;goto remap_op;
        case EFI_MEMORY_MAPPED_IO:
        case EFI_MEMORY_MAPPED_IO_PORT_SPACE:
        p.cache_strateggy=cache_strategy_t::UC;
        p.is_writable=1;
        p.is_executable=0;goto remap_op;
        case EFI_RUNTIME_SERVICES_DATA:
        p.cache_strateggy=cache_strategy_t::WB;
        p.is_writable=1;
        p.is_executable=0;goto remap_op;
        case EFI_RUNTIME_SERVICES_CODE:
        p.cache_strateggy=cache_strategy_t::WB;
        p.is_writable=1;
        p.is_executable=1;goto remap_op;


remap_op:
        phy_memDesTb[i].VirtualStart=(EFI_VIRTUAL_ADDRESS)
        pgs_remapp(
            phy_memDesTb[i].PhysicalStart,
            p
        );
        if(phy_memDesTb[i].VirtualStart==NULL){
            kputsSecure("remap_op:pgs_remapp failed");
            return;
        }
        default:
            continue;
        }
     }
     
}
const inline   uint16_t pagesnum_to_max_entry_count(uint64_t num_of_4kbpgs)
{
    return num_of_4kbpgs>4096?4096:num_of_4kbpgs;
}
KernelSpacePgsMemMgr::phymemSegsSubMgr_t::phymemSegsSubMgr_t()
{//从第1mb开始，低1mb内存的内容不能自由分配，其它子管理器进行管理
    allocatable_mem_seg_count=0;
    setmem(&allocatable_mem_seg[0],sizeof(allocatable_mem_seg_t)*max_entry_count,0);
    phy_memDescriptor*base=gBaseMemMgr.getGlobalPhysicalMemoryInfo();
    phy_memDescriptor*end=base+gBaseMemMgr.getRootPhysicalMemoryDescriptorTableEntryCount();
    phy_memDescriptor*scan_start=gBaseMemMgr.queryPhysicalMemoryUsage(0x100000);
    phy_memDescriptor*scan_index=scan_start;
    while (scan_index<end)
    { 
       if(scan_index->Type==freeSystemRam)
            {
                // 检查是否超出最大条目数
                if (allocatable_mem_seg_count >= max_entry_count) {
                    kputsSecure("phymemSegsSubMgr_t: too many memory segments\n");
                    break;
                }

                phyaddr_t seg_base = (scan_index==scan_start) ? 0x100000 : scan_index->PhysicalStart;
                phyaddr_t seg_end = scan_index->PhysicalStart + scan_index->NumberOfPages * 0x1000;
                uint64_t seg_size = (seg_end - seg_base) >> 12;
                
                allocatable_mem_seg[allocatable_mem_seg_count].base = seg_base;
                allocatable_mem_seg[allocatable_mem_seg_count].size_in_numof4kbpgs = seg_size;
                allocatable_mem_seg[allocatable_mem_seg_count].max_num_of_subtb_entries =
                    pagesnum_to_max_entry_count(allocatable_mem_seg[allocatable_mem_seg_count].size_in_numof4kbpgs);
                allocatable_mem_seg[allocatable_mem_seg_count].num_of_subtb_entries = 0;
                
                // 分配子表内存
                allocatable_mem_seg[allocatable_mem_seg_count].subtb =
                    new minimal_phymem_seg_t[allocatable_mem_seg[allocatable_mem_seg_count].max_num_of_subtb_entries];
                
                // 检查内存分配是否成功
                if (allocatable_mem_seg[allocatable_mem_seg_count].subtb == nullptr) {
                    kputsSecure("phymemSegsSubMgr_t: failed to allocate memory for subtb\n");
                    // 释放之前分配的内存
                    for (int i = 0; i < allocatable_mem_seg_count; i++) {
                        delete[] allocatable_mem_seg[i].subtb;
                    }
                    allocatable_mem_seg_count = 0;
                    break;
                }
                
                allocatable_mem_seg_count++;
            }
        scan_index++;
    }
}
void KernelSpacePgsMemMgr::phymemSegsSubMgr_t::Init()
{
        allocatable_mem_seg_count=0;
    setmem(&allocatable_mem_seg[0],sizeof(allocatable_mem_seg_t)*max_entry_count,0);
    phy_memDescriptor*base=gBaseMemMgr.getGlobalPhysicalMemoryInfo();
    phy_memDescriptor*end=base+gBaseMemMgr.getRootPhysicalMemoryDescriptorTableEntryCount();
    phy_memDescriptor*scan_start=gBaseMemMgr.queryPhysicalMemoryUsage(0x100000);
    phy_memDescriptor*scan_index=scan_start;
    while (scan_index<end)
    { 
       if(scan_index->Type==freeSystemRam)
            {
                // 检查是否超出最大条目数
                if (allocatable_mem_seg_count >= max_entry_count) {
                    kputsSecure("phymemSegsSubMgr_t: too many memory segments\n");
                    break;
                }

                phyaddr_t seg_base = (scan_index==scan_start) ? 0x100000 : scan_index->PhysicalStart;
                phyaddr_t seg_end = scan_index->PhysicalStart + scan_index->NumberOfPages * 0x1000;
                uint64_t seg_size = (seg_end - seg_base) >> 12;
                
                allocatable_mem_seg[allocatable_mem_seg_count].base = seg_base;
                allocatable_mem_seg[allocatable_mem_seg_count].size_in_numof4kbpgs = seg_size;
                allocatable_mem_seg[allocatable_mem_seg_count].max_num_of_subtb_entries =
                    pagesnum_to_max_entry_count(allocatable_mem_seg[allocatable_mem_seg_count].size_in_numof4kbpgs);
                allocatable_mem_seg[allocatable_mem_seg_count].num_of_subtb_entries = 0;
                
                // 分配子表内存
                allocatable_mem_seg[allocatable_mem_seg_count].subtb =
                    new minimal_phymem_seg_t[allocatable_mem_seg[allocatable_mem_seg_count].max_num_of_subtb_entries];
                
                // 检查内存分配是否成功
                if (allocatable_mem_seg[allocatable_mem_seg_count].subtb == nullptr) {
                    kputsSecure("phymemSegsSubMgr_t: failed to allocate memory for subtb\n");
                    // 释放之前分配的内存
                    for (int i = 0; i < allocatable_mem_seg_count; i++) {
                        delete[] allocatable_mem_seg[i].subtb;
                    }
                    allocatable_mem_seg_count = 0;
                    break;
                }
                
                allocatable_mem_seg_count++;
            }
        scan_index++;
    }
}
int KernelSpacePgsMemMgr::PgCBtb_lv4_entry_construct(phyaddr_t addr, pgflags flags, phyaddr_t mapped_phyaddr)
{
    int status=0;
    uint64_t lv4_index=(addr&lineaddr_index_filters::PML5_INDEX_MASK_lv4)>>48;
    lv4_index=cpu_pglv==5?lv4_index:0;
    flags.pg_lv=4;
    rootlv4PgCBtb[lv4_index].flags = flags;
    if (flags.is_atom==0)
    {
      
            rootlv4PgCBtb[lv4_index].base.lowerlvPgCBtb= new lowerlv_PgCBtb;
            if (rootlv4PgCBtb[lv4_index].base.lowerlvPgCBtb==nullptr)
            {
                return OS_OUT_OF_MEMORY;
            }
            gKpoolmemmgr.clear(rootlv4PgCBtb[lv4_index].base.lowerlvPgCBtb);
        
        
    }
    
    return OS_SUCCESS;
}

int KernelSpacePgsMemMgr::PgCBtb_lv3_entry_construct(phyaddr_t addr, pgflags flags,phyaddr_t mapped_phyaddr)
{

    uint16_t lv3_index=(addr&lineaddr_index_filters:: PML4_INDEX_MASK_lv3)>>39;
    uint64_t lv4_index=(addr&lineaddr_index_filters:: PML5_INDEX_MASK_lv4)>>48;
    PgCBlv4header* lv4_PgCBHeader = cpu_pglv == 5 ? &rootlv4PgCBtb[lv4_index] : rootlv4PgCBtb;
    pgflags higher_uninitialized_entry_flags;
    higher_uninitialized_entry_flags.is_exist=1;
    higher_uninitialized_entry_flags.is_atom=0;
    higher_uninitialized_entry_flags.cache_strateggy=cache_strategy_t::WB;
    higher_uninitialized_entry_flags.physical_or_virtual_pg=0;
    higher_uninitialized_entry_flags.is_kernel=1;
    // 确保lv4条目存在
    if (lv4_PgCBHeader->flags.is_exist != 1) {
 
        higher_uninitialized_entry_flags.pg_lv = 3;
    
        int status = PgCBtb_lv4_entry_construct(addr, higher_uninitialized_entry_flags);
        if (status != OS_SUCCESS)
            return status;
    }

    PgCBlv3header* lv3_PgCBHeader = &lv4_PgCBHeader->base.lowerlvPgCBtb->entries[lv3_index];

    // 初始化lv3条目
    lv3_PgCBHeader->flags = flags;
     if (flags.is_atom==0)
    {
       
            lv3_PgCBHeader->base.lowerlvPgCBtb= new lowerlv_PgCBtb;
            if (lv3_PgCBHeader->base.lowerlvPgCBtb==nullptr)
            {
                return OS_OUT_OF_MEMORY;
            }
            gKpoolmemmgr.clear(lv3_PgCBHeader->base.lowerlvPgCBtb);
        
        
    }
    
    return OS_SUCCESS;
}
int KernelSpacePgsMemMgr::PgCBtb_lv2_entry_construct(phyaddr_t addr, pgflags flags,phyaddr_t mapped_phyaddr)
{

    uint16_t lv2_index=(addr&lineaddr_index_filters:: PDPT_INDEX_MASK_lv2)>>30;
    uint16_t lv3_index=(addr&lineaddr_index_filters:: PML4_INDEX_MASK_lv3)>>39;
    uint64_t lv4_index=(addr&lineaddr_index_filters:: PML5_INDEX_MASK_lv4)>>48;
    int status=0;
    PgCBlv4header* lv4_PgCBHeader = cpu_pglv == 5 ? &rootlv4PgCBtb[lv4_index] : rootlv4PgCBtb;
    pgflags higher_uninitialized_entry_flags;
    higher_uninitialized_entry_flags.is_exist = 1;
    higher_uninitialized_entry_flags.is_atom = 0;
    higher_uninitialized_entry_flags.cache_strateggy = cache_strategy_t::WB;
    higher_uninitialized_entry_flags.physical_or_virtual_pg = 0;
    higher_uninitialized_entry_flags.is_kernel = 1;

    // 确保lv4条目存在
    if (lv4_PgCBHeader->flags.is_exist != 1) {
        higher_uninitialized_entry_flags.pg_lv = 4;
        int status = PgCBtb_lv4_entry_construct(addr, higher_uninitialized_entry_flags);
        if (status != OS_SUCCESS)
            return status;
    }

    // 确保lv3条目存在
    PgCBlv3header* lv3_PgCBHeader = &lv4_PgCBHeader->base.lowerlvPgCBtb->entries[lv3_index];
    if (lv3_PgCBHeader->flags.is_exist != 1) {
        higher_uninitialized_entry_flags.pg_lv = 3;
        int status = PgCBtb_lv3_entry_construct(addr, higher_uninitialized_entry_flags);
        if (status != OS_SUCCESS)
            return status;
    }else{
        if(lv3_PgCBHeader->flags.is_atom==1)
        {
            lv3_PgCBHeader->flags.is_atom=0;
            lv3_PgCBHeader->base.lowerlvPgCBtb= new lowerlv_PgCBtb;
            gKpoolmemmgr.clear(lv3_PgCBHeader->base.lowerlvPgCBtb);
        }
    }
    PgCBlv2header* lv2_PgCBHeader = &lv3_PgCBHeader->base.lowerlvPgCBtb->entries[lv2_index];
    lv2_PgCBHeader->flags = flags;
    if(lv2_PgCBHeader->flags.is_atom==0)
    {
        lv2_PgCBHeader->base.lowerlvPgCBtb= new lowerlv_PgCBtb;
        if (lv2_PgCBHeader->base.lowerlvPgCBtb==nullptr)
        {
            kputsSecure("new lowerlv_PgCBtb failed");
            return OS_OUT_OF_MEMORY;
        }
        
        gKpoolmemmgr.clear(lv2_PgCBHeader->base.lowerlvPgCBtb);

    }else
    {
        if(lv2_PgCBHeader->flags.physical_or_virtual_pg==VIR_ATOM_PAGE)
        {
            lv2_PgCBHeader->base.base_phyaddr=mapped_phyaddr;
        }
    }
    
    return OS_SUCCESS;
}

int KernelSpacePgsMemMgr::PgCBtb_lv1_entry_construct(phyaddr_t addr, pgflags flags,phyaddr_t mapped_phyaddr)
{

    uint16_t lv1_index=(addr&lineaddr_index_filters:: PD_INDEX_MASK_lv1)>>21;
    uint16_t lv2_index=(addr&lineaddr_index_filters:: PDPT_INDEX_MASK_lv2)>>30;
    uint16_t lv3_index=(addr&lineaddr_index_filters:: PML4_INDEX_MASK_lv3)>>39;
    uint64_t lv4_index=(addr&lineaddr_index_filters:: PML5_INDEX_MASK_lv4)>>48;
    int status=0;
    PgCBlv4header* lv4_PgCBHeader = cpu_pglv == 5 ? &rootlv4PgCBtb[lv4_index] : rootlv4PgCBtb;
    pgflags higher_uninitialized_entry_flags;
    higher_uninitialized_entry_flags.is_exist = 1;
    higher_uninitialized_entry_flags.is_atom = 0;
    higher_uninitialized_entry_flags.physical_or_virtual_pg = 0;
    higher_uninitialized_entry_flags.is_kernel = 1;
     higher_uninitialized_entry_flags.cache_strateggy=cache_strategy_t::WB;
    // 确保lv4条目存在
    if (lv4_PgCBHeader->flags.is_exist != 1) {
        higher_uninitialized_entry_flags.pg_lv = 4;
        int status = PgCBtb_lv4_entry_construct(addr, higher_uninitialized_entry_flags);
        if (status != OS_SUCCESS)
            return status;
    }

    // 确保lv3条目存在
    PgCBlv3header* lv3_PgCBHeader = &lv4_PgCBHeader->base.lowerlvPgCBtb->entries[lv3_index];
    if (lv3_PgCBHeader->flags.is_exist != 1) {
        higher_uninitialized_entry_flags.pg_lv = 3;
        int status = PgCBtb_lv3_entry_construct(addr, higher_uninitialized_entry_flags);
        if (status != OS_SUCCESS)
            return status;
    }else{
        if(lv3_PgCBHeader->flags.is_atom==1)
        {
            lv3_PgCBHeader->flags.is_atom=0;
            status=PgCBtb_lv2_entry_construct(addr,lv3_PgCBHeader->flags);
            
        }
    }

    // 确保lv2条目存在
    PgCBlv2header* lv2_PgCBHeader = &lv3_PgCBHeader->base.lowerlvPgCBtb->entries[lv2_index];
    if (lv2_PgCBHeader->flags.is_exist != 1) {
        higher_uninitialized_entry_flags.pg_lv = 2;
        int status = PgCBtb_lv2_entry_construct(addr, higher_uninitialized_entry_flags);
        if (status != OS_SUCCESS)
            return status;
    }else{
        if(lv2_PgCBHeader->flags.is_atom==1)
        {
            lv2_PgCBHeader->flags.is_atom=0;
            lv2_PgCBHeader->base.lowerlvPgCBtb= new lowerlv_PgCBtb;
            gKpoolmemmgr.clear(lv2_PgCBHeader->base.lowerlvPgCBtb);
        }
    }

    PgCBlv1header* lv1_PgCBHeader = &lv2_PgCBHeader->base.lowerlvPgCBtb->entries[lv1_index];
    lv1_PgCBHeader->flags = flags;
    if(lv1_PgCBHeader->flags.is_atom==0)
    {
        lv1_PgCBHeader->base.lowerlvPgCBtb= new lowerlv_PgCBtb;
        if (lv1_PgCBHeader->base.lowerlvPgCBtb==nullptr)
        {
            kputsSecure("new lowerlv_PgCBtb failed");
            return OS_OUT_OF_MEMORY;
        }
        
        gKpoolmemmgr.clear(lv1_PgCBHeader->base.lowerlvPgCBtb);
    }else
    {
        if(lv1_PgCBHeader->flags.physical_or_virtual_pg==VIR_ATOM_PAGE)
        lv1_PgCBHeader->base.base_phyaddr=mapped_phyaddr;
    }
    
    

    return OS_SUCCESS;
}


int KernelSpacePgsMemMgr::PgCBtb_lv0_entry_construct(phyaddr_t addr, pgflags flags,phyaddr_t mapped_phyaddr)
{
    int status=0;
    uint16_t lv0_index=(addr&lineaddr_index_filters:: PT_INDEX_MASK_lv0)>>12;
    uint16_t lv1_index=(addr&lineaddr_index_filters:: PD_INDEX_MASK_lv1)>>21;
    uint16_t lv2_index=(addr&lineaddr_index_filters:: PDPT_INDEX_MASK_lv2)>>30;
    uint16_t lv3_index=(addr&lineaddr_index_filters:: PML4_INDEX_MASK_lv3)>>39;
    uint64_t lv4_index=(addr&lineaddr_index_filters:: PML5_INDEX_MASK_lv4)>>48;
    pgflags higher_uninitialized_entry_flags;
    higher_uninitialized_entry_flags.is_exist=1;
    higher_uninitialized_entry_flags.is_atom=0;
    higher_uninitialized_entry_flags.cache_strateggy=cache_strategy_t::WB;
    higher_uninitialized_entry_flags.physical_or_virtual_pg=0;
    higher_uninitialized_entry_flags.is_kernel=1;
    PgCBlv4header*lv4_PgCBHeader=cpu_pglv==5?&rootlv4PgCBtb[lv4_index]:rootlv4PgCBtb;
    if(lv4_PgCBHeader->flags.is_exist!=1){
        
        higher_uninitialized_entry_flags.pg_lv=4;
        status=PgCBtb_lv4_entry_construct(addr,higher_uninitialized_entry_flags);
        if (status!=OS_SUCCESS)
        {
            return status;
        }
        
    }
    PgCBlv3header*lv3_PgCBHeader=&lv4_PgCBHeader->base.lowerlvPgCBtb->entries[lv3_index];
    if(lv3_PgCBHeader->flags.is_exist!=1){
        higher_uninitialized_entry_flags.pg_lv=3;
        status=PgCBtb_lv3_entry_construct(addr,higher_uninitialized_entry_flags);
        if (status!=OS_SUCCESS)
        {
            return status;
        }
    }else{
        if(lv3_PgCBHeader->flags.is_atom==1)
        {
            lv3_PgCBHeader->flags.is_atom=0;
            status=PgCBtb_lv2_entry_construct(addr,lv3_PgCBHeader->flags);
            if(status!=OS_SUCCESS)
            {
                kputsSecure("PgCBtb_lv0_entry_construct:PgCBtb_lv2_entry_construct failed");
                return OS_INVALID_ADDRESS;
            }
        }
    }
    PgCBlv2header*lv2_PgCBHeader=&lv3_PgCBHeader->base.lowerlvPgCBtb->entries[lv2_index];
    if(lv2_PgCBHeader->flags.is_exist!=1){
        higher_uninitialized_entry_flags.pg_lv=2;
        status=PgCBtb_lv2_entry_construct(addr,higher_uninitialized_entry_flags);
        if (status!=OS_SUCCESS)
        {
            return status;
        }
    }else{
        if(lv2_PgCBHeader->flags.is_atom==1)
        {
            lv2_PgCBHeader->flags.is_atom=0;
            status=PgCBtb_lv1_entry_construct(addr,lv2_PgCBHeader->flags);
            if(status!=OS_SUCCESS) 
            {
                kputsSecure("PgCBtb_lv0_entry_construct:PgCBtb_lv1_entry_construct failed");
                return OS_INVALID_ADDRESS;
            } 
        }
    }
    PgCBlv1header*lv1_PgCBHeader=&lv2_PgCBHeader->base.lowerlvPgCBtb->entries[lv1_index];
    if(lv1_PgCBHeader->flags.is_exist!=1){
        higher_uninitialized_entry_flags.pg_lv=1;
        status=PgCBtb_lv1_entry_construct(addr,higher_uninitialized_entry_flags);
        if (status!=OS_SUCCESS)
        {
            return status;
        }
    }else{
        if(lv1_PgCBHeader->flags.is_atom==1)
        {
            lv1_PgCBHeader->flags.is_atom=0;
            status=PgCBtb_lv1_entry_construct(addr,lv1_PgCBHeader->flags);
            if(status!=OS_SUCCESS)
            {
                kputsSecure("PgCBtb_lv0_entry_construct:PgCBtb_lv1_entry_construct failed");
                return OS_INVALID_ADDRESS;
            }
            
        }
    }
    PgCBlv0header*lv0_PgCBHeader=&lv1_PgCBHeader->base.lowerlvPgCBtb->entries[lv0_index];
    lv0_PgCBHeader->flags=flags;
    if(lv0_PgCBHeader->flags.physical_or_virtual_pg==VIR_ATOM_PAGE)
    {
        lv0_PgCBHeader->base.base_phyaddr=mapped_phyaddr;
    }
    return OS_SUCCESS;
}
int KernelSpacePgsMemMgr::construct_pgsbasedon_phy_memDescriptor(phy_memDescriptor memDescriptor)
{
    int status;
    PHY_MEM_TYPE type = (PHY_MEM_TYPE)memDescriptor.Type;
    phyaddr_t base = memDescriptor.PhysicalStart;
    uint64_t numof_4kbgs = memDescriptor.NumberOfPages;
    uint64_t seg_flags = memDescriptor.Attribute;
    phyaddr_t end_addr = base + numof_4kbgs * PAGE_SIZE_IN_LV[0] ; // 结束地址是最后一个字节的地址
    phyaddr_t scan_addr;
    phymem_pgs_queue* queue =KernelSpacePgsMemMgr:: seg_to_queue(base,numof_4kbgs * PAGE_SIZE_IN_LV[0]);

    // 设置页标志
    pgflags flags;
    flags.physical_or_virtual_pg = 0; // 物理页
    flags.is_exist = 1; // 存在
    flags.is_atom = 1; // 原子节点
    flags.is_global=0;
    flags.cache_strateggy=cache_strategy_t::WB;
// 设置保留标志 - 不可分配的内存
flags.is_reserved = (type == EFI_RESERVED_MEMORY_TYPE || 
                    type == EFI_UNUSABLE_MEMORY || 
                    type == EFI_ACPI_MEMORY_NVS ||
                    type == EFI_MEMORY_MAPPED_IO ||
                    type == EFI_MEMORY_MAPPED_IO_PORT_SPACE ||
                    type == EFI_PAL_CODE ||
                    type == MEMORY_TYPE_OEM_RESERVED_MIN || // OEM保留范围
                    type == MEMORY_TYPE_OS_RESERVED_MIN ||  // OS保留范围
                    (type >= MEMORY_TYPE_OEM_RESERVED_MIN && type <= MEMORY_TYPE_OEM_RESERVED_MAX) ||
                    (type >= MEMORY_TYPE_OS_RESERVED_MIN && type <= MEMORY_TYPE_OS_RESERVED_MAX)) 
                    ? 1 : 0;

// 设置占用标志 - 是否已被使用
flags.is_occupied = (type == freeSystemRam || 
                    type == EFI_UNACCEPTED_MEMORY_TYPE) 
                    ? 0 : 1; // 只有空闲内存和未接受内存是可用的

flags.is_kernel = 1; // 物理内存默认为内核内存
flags.is_readable = 1; // 默认可读

// 设置可写标志
flags.is_writable = (type == EFI_LOADER_DATA || 
                    type == EFI_BOOT_SERVICES_DATA || 
                    type == EFI_RUNTIME_SERVICES_DATA || 
                    type == EFI_RUNTIME_SERVICES_CODE ||
                    type == freeSystemRam ||
                    type == EfiReservedMemoryType || 
                    type == EFI_UNACCEPTED_MEMORY_TYPE ||
                    type == EFI_ACPI_RECLAIM_MEMORY) 
                    ? 1 : 0;
// 设置可执行标志 - 严格控制执行权限
flags.is_executable = (type == EFI_LOADER_CODE || 
                      type == EFI_BOOT_SERVICES_CODE || 
                      type == EFI_RUNTIME_SERVICES_CODE || 
                      type == EFI_PAL_CODE) 
                      ? 1 : 0;
if(type==EFI_RUNTIME_SERVICES_CODE||type==EFI_RUNTIME_SERVICES_DATA)
flags.cache_strateggy=cache_strategy_t::WT;
// 特殊处理ACPI内存
if (type == EFI_ACPI_RECLAIM_MEMORY||type==OS_PGTB_SEGS) {
    // ACPI回收内存：可读写但不可执行
    flags.is_writable = 1;
    flags.is_executable = 0;
    flags.cache_strateggy=cache_strategy_t::WT;
}

if (type == EFI_ACPI_MEMORY_NVS) {
    // ACPI NVS内存：只读，不可写不可执行
    flags.is_writable = 0;
    flags.is_executable = 0;
    flags.cache_strateggy=cache_strategy_t::UC;
}

if (type == EFI_MEMORY_MAPPED_IO || type == EFI_MEMORY_MAPPED_IO_PORT_SPACE) {
    // MMIO区域：根据架构可能需要特殊处理
    // 通常可读写但不可执行
    flags.is_writable = 1;
    flags.is_executable = 0;
    flags.is_reserved = 1; // MMIO必须保留
    flags.cache_strateggy=cache_strategy_t::UC;
}

if (type == EFI_PERSISTENT_MEMORY) {
    // 持久内存：可读写，是否可执行取决于用途
    flags.is_writable = 1;
    flags.is_executable = 0; // 默认不可执行，需要时再设置
}


    
    // 根据队列创建页表项
    scan_addr = base;
    for (int i = 0; i < queue->entry_count; i++)
    {
        for (int j = 0; j < queue->entry[i].pgs_count; j++)
        {
            flags.pg_lv = queue->entry[i].pgs_lv;
            
  // 使用函数指针数组调用对应的构造函数
            if (queue->entry[i].pgs_lv >= 0 && queue->entry[i].pgs_lv <= 4)
            {
                status = (this->*PgCBtb_construct_func[queue->entry[i].pgs_lv])(scan_addr, flags,0);
            }
            else
            {
                status = OS_INVALID_PARAMETER;
            }
            
            if (status != OS_SUCCESS)
            {
                gKpoolmemmgr.print_meta_table(gKpoolmemmgr.getFirst_static_heap());
                delete queue;
                return status;
            }
            
            scan_addr += PAGE_SIZE_IN_LV[queue->entry[i].pgs_lv];
        }
    }
    
    delete queue;
    return OS_SUCCESS;
}
