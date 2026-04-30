#pragma once
#include "stdint.h"
#include "util/kptrace.h"
constexpr uint64_t KMODS_MAGIC = 0x223FBB2F6E730806;
struct kmods_metadata_header
{
    uint64_t magic;
    uint64_t version;
    uint32_t require_symbols_table_offset;
    uint32_t require_symbols_table_entry_count;
    uint32_t export_symbols_table_offset;
    uint32_t export_symbols_table_entry_count;
};