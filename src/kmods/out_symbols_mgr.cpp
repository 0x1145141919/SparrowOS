#include "kmods/out_symbols_mgr.h"
#include "util/OS_utils.h"
int symbol_entry_name_bin_cmp(const symbol_entry &a, const symbol_entry &b)
{
    return strcmp_in_kernel(a.name, b.name,119);
}