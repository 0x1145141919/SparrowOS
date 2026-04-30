#include "util/kptrace.h"
#include "util/Ktemplats.h"
int symbol_entry_name_bin_cmp(const symbol_entry& a,const symbol_entry& b);
class dump_symbol_manager:Ktemplats::RBTree<symbol_entry, symbol_entry_name_bin_cmp>
{
    
};