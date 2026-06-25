#include "phygpsmemmgr.h"
#include "VideoDriver.h"

void KernelSpacePgsMemMgr::PrintPgsMemMgrStructure()
{
    kputsSecure("=== Page Table Structure ===\n");
    
    // 打印基本信息
    kputsSecure("CPU Page Level: ");
    kputsSecure(" (");
    kpnumSecure(&cpu_pglv, UNHEX, 1);
    kputsSecure(")\n");
    

    

    
    // 打印各级页表结构
    if (cpu_pglv == 5)
    {
        // 五级页表模式
        for (int i = 0; i < 512; i++)
        {
            if (rootlv4PgCBtb[i].flags.is_exist)
            {
                kputsSecure("PML5[");
                kpnumSecure(&i, UNDEC, 3);
                kputsSecure("]: ");
                PrintPageTableEntry(&rootlv4PgCBtb[i], 4);
                
                // 打印下一级
                if (!rootlv4PgCBtb[i].flags.is_atom && rootlv4PgCBtb[i].base.lowerlvPgCBtb)
                {
                    PrintLevel4Table(rootlv4PgCBtb[i].base.lowerlvPgCBtb, i);
                }
                kputsSecure("\n");
            }
        }
    }
    else
    {
        // 四级页表模式
        kputsSecure("PML4[0]: ");
        PrintPageTableEntry(rootlv4PgCBtb, 4);
        
        // 打印下一级
        if (!rootlv4PgCBtb->flags.is_atom && rootlv4PgCBtb->base.lowerlvPgCBtb)
        {
            PrintLevel4Table(rootlv4PgCBtb->base.lowerlvPgCBtb, 0);
        }
        kputsSecure("\n");
    }
}



// 辅助函数：打印四级表
void KernelSpacePgsMemMgr::PrintLevel4Table(lowerlv_PgCBtb* table, int parentIndex)
{
    for (int i = 0; i < 512; i++)
    {
        if ((table->entries[i]).flags.is_exist)
        {
            kputsSecure("  PML4[");
            kpnumSecure(&parentIndex, UNDEC, 3);
            kputsSecure("]->PDPT[");
            kpnumSecure(&i, UNDEC, 3);
            kputsSecure("]: ");
            PrintPageTableEntry(&table->entries[i], 3);
            
            // 打印下一级
             if (!table->entries[i].flags.is_atom && table->entries[i].base.lowerlvPgCBtb)
            {
                PrintLevel3Table(table->entries[i].base.lowerlvPgCBtb, parentIndex, i);
            }
            kputsSecure("\n");
        }
    }
}

// 辅助函数：打印三级表
void KernelSpacePgsMemMgr::PrintLevel3Table(lowerlv_PgCBtb* table, int grandParentIndex, int parentIndex)
{
    for (int i = 0; i < 512; i++)
    {
        if (table->entries[i].flags.is_exist)
        {
            kputsSecure("    PDPT[");
            kpnumSecure(&parentIndex, UNDEC, 3);
            kputsSecure("]->PD[");
            kpnumSecure(&i, UNDEC, 3);
            kputsSecure("]: ");
            PrintPageTableEntry(&table->entries[i], 2);
            
 if (!table->entries[i].flags.is_atom && table->entries[i].base.lowerlvPgCBtb)
            {
                PrintLevel2Table(table->entries[i].base.lowerlvPgCBtb, grandParentIndex, parentIndex, i);
            }
            kputsSecure("\n");
        }
    }
}

// 辅助函数：打印二级表
void KernelSpacePgsMemMgr::PrintLevel2Table(lowerlv_PgCBtb* table, int greatGrandParentIndex, int grandParentIndex, int parentIndex)
{
    for (int i = 0; i < 512; i++)
    {
        if (table->entries[i].flags.is_exist)
        {
            kputsSecure("      PD[");
            kpnumSecure(&parentIndex, UNDEC, 3);
            kputsSecure("]->PT[");
            kpnumSecure(&i, UNDEC, 3);
            kputsSecure("]: ");
            PrintPageTableEntry(&table->entries[i], 1);
            
 if (!table->entries[i].flags.is_atom && table->entries[i].base.lowerlvPgCBtb)
            {
                PrintLevel1Table(table->entries[i].base.lowerlvPgCBtb, greatGrandParentIndex, grandParentIndex, parentIndex, i);
            }
            kputsSecure("\n");
        }
    }
}

// 辅助函数：打印一级表
void KernelSpacePgsMemMgr::PrintLevel1Table(lowerlv_PgCBtb* table, int greatGreatGrandParentIndex, int greatGrandParentIndex, int grandParentIndex, int parentIndex)
{
    for (int i = 0; i < 512; i++)
    {
        if (table->entries[i].flags.is_exist)
        {
            kputsSecure("        PT[");
            kpnumSecure(&parentIndex, UNDEC, 3);
            kputsSecure("]->Page[");
            kpnumSecure(&i, UNDEC, 3);
            kputsSecure("]: ");
            PrintPageTableEntry(&table->entries[i], 0);
            
            // 计算并打印物理地址
            uint64_t physicalAddr = CalculatePhysicalAddress(
                greatGreatGrandParentIndex, greatGrandParentIndex, 
                grandParentIndex, parentIndex, i, 0);
            
            kputsSecure(" PhysAddr: ");
            kpnumSecure(&physicalAddr, UNHEX, 8);
            kputsSecure("\n");
        }
    }
}

// 辅助函数：计算物理地址
uint64_t KernelSpacePgsMemMgr::CalculatePhysicalAddress(int index5, int index4, int index3, int index2, int index1, int index0)
{
    uint64_t addr = 0;
    
    if (cpu_pglv == 5)
    {
        addr |= (static_cast<uint64_t>(index5) << 48);
    }
    
    addr |= (static_cast<uint64_t>(index4) << 39);
    addr |= (static_cast<uint64_t>(index3) << 30);
    addr |= (static_cast<uint64_t>(index2) << 21);
    addr |= (static_cast<uint64_t>(index1) << 12);
    addr |= (static_cast<uint64_t>(index0));
    
    return addr;
}

// 辅助函数：打印页表项信息
void KernelSpacePgsMemMgr::PrintPageTableEntry(PgControlBlockHeader* entry, int level)
{
    // 打印级别
    kputsSecure((char*)"L");
    kpnumSecure(&level, UNDEC, 1);
    kputsSecure((char*)": ");
    
    // 打印基本标志
    kputsSecure(entry->flags.physical_or_virtual_pg ? (char*)"Virtual " : (char*)"Physical ");
    kputsSecure(entry->flags.is_exist ? (char*)"Exist " : (char*)"NotExist ");
    kputsSecure(entry->flags.is_atom ? (char*)"Atom " : (char*)"NonAtom ");

    
    
    kputsSecure(entry->flags.is_reserved ? (char*)"Reserved " : (char*)"NonReserved ");
    
    // 打印权限标志
    kputsSecure(entry->flags.is_readable ? (char*)"R" : (char*)"-");
    kputsSecure(entry->flags.is_writable ? (char*)"W" : (char*)"-");
    kputsSecure(entry->flags.is_executable ? (char*)"X" : (char*)"-");
    kputsSecure((char*)" ");
    
    // 打印其他标志
    kputsSecure(entry->flags.is_kernel ? (char*)"Kernel " : (char*)"User ");
    kputsSecure(entry->flags.is_occupied ? (char*)"Occupied " : (char*)"Free ");
}

// 辅助函数：打印1bit位图


// 打印pgflags结构
void print_pgflags(pgflags flags) {
    kputsSecure("Flags:\n");
    
    // 打印标志位
    kputsSecure("  physical_or_virtual_pg: ");

    kputsSecure((bool)flags.physical_or_virtual_pg ? (char*)" (Virtual)" : (char*)" (Physical)\n");
    
    kputsSecure("  is_exist: ");
    kputcharSecure(flags.is_exist ? '1' : '0');
    kputsSecure("\n");
    
    kputsSecure("  is_atom: ");
    kputcharSecure(flags.is_atom ? '1' : '0');
    kputsSecure("\n");

    kputsSecure("  is_reserved: ");
    kputcharSecure(flags.is_reserved ? '1' : '0');
    kputsSecure("\n");
    
    kputsSecure("  is_occupied: ");
    kputsSecure(flags.is_occupied ? (char*)" (Occupied)" : (char*)" (Free)");
    kputsSecure("\n");
    
    kputsSecure("  is_kernel: ");
    kputcharSecure(flags.is_kernel ? '1' : '0');
    kputsSecure("\n");
    
    kputsSecure("  is_readable: ");
    kputcharSecure  (flags.is_readable ? '1' : '0');
    kputsSecure("\n");
    
    kputsSecure("  is_writable: ");
    kputcharSecure(flags.is_writable ? '1' : '0');
    kputsSecure("\n");
    
    kputsSecure("  is_executable: ");
    kputcharSecure(flags.is_executable ? '1' : '0');
    kputsSecure("\n");
    
    kputsSecure("  is_remaped: ");
    kputcharSecure(flags.is_remaped ? '1' : '0');
    kputsSecure("\n");
    
    kputsSecure("  pg_lv: ");
    uint8_t pg_lv = flags.pg_lv;
    kpnumSecure(&pg_lv, UNDEC, 1);
    kputsSecure("\n");
}

// 打印1位宽位图

// 打印2位宽位图

void print_PgControlBlockHeader(struct PgControlBlockHeader* header) {
    if (!header) {
        kputsSecure("Invalid PgControlBlockHeader pointer\n");
        return;
    }
    
    kputsSecure("==== PgControlBlockHeader ====\n");
    
    // 打印标志
    print_pgflags(header->flags);
    
    // 打印base联合体
    kputsSecure("Base:\n");
    
    if (!header->flags.is_exist) {
        kputsSecure("  Page not exists\n");
        return;
    }
    
    if (header->flags.is_atom) {
        kputsSecure("  Atomic node - no lower levels\n");
        return;
    }
    

        kputsSecure("  Lower level table (lowerlv_PgCBtb):\n");
        if (header->base.lowerlvPgCBtb) {
            kputsSecure("    Table exists\n");
            kputsSecure("    pgextention: ");
            kpnumSecure(&header->base.lowerlvPgCBtb->pgextention, UNHEX, 8);
            kputsSecure("\n");
        } else {
            kputsSecure("    Table pointer is NULL\n");
        }
    
}

