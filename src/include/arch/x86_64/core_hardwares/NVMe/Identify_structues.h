#pragma once
#include <stdint.h>

namespace NVMe {

struct __attribute__((packed)) ns_list_t {
    uint32_t nsid[1024];
};
static_assert(sizeof(ns_list_t) == 4096, "ns_list_t must be 4096 bytes");

struct __attribute__((packed)) lbaf_entry_t {
    uint16_t ms;
    uint8_t  lbads;
    uint8_t  rp_padding;
};
static_assert(sizeof(lbaf_entry_t) == 4, "lbaf_entry_t must be 4 bytes");

// Identify Namespace Data Structure (NVM Cmd Set) - 4096 bytes
struct __attribute__((packed)) identify_ns_nvm_t {
    uint64_t nsze;            // 000h
    uint64_t ncap;            // 008h
    uint64_t nuse;            // 010h
    uint8_t  nsfeat;          // 018h
    uint8_t  nlbaf;           // 019h
    uint8_t  flbas;           // 01Ah
    uint8_t  mc;              // 01Bh
    uint8_t  dpc;             // 01Ch
    uint8_t  dps;             // 01Dh
    uint8_t  nmic;            // 01Eh
    uint8_t  rescap;          // 01Fh
    uint8_t  fpi;             // 020h
    uint8_t  dlfeat;          // 021h
    uint16_t nawun;           // 022h
    uint16_t nawupf;          // 024h
    uint16_t nacwu;           // 026h
    uint16_t nabsn;           // 028h
    uint16_t nabo;            // 02Ah
    uint16_t nabspf;          // 02Ch
    uint16_t noiob;           // 02Eh
    uint64_t nvmcap[2];       // 030h
    uint64_t npwg;            // 040h
    uint64_t npwa;            // 048h
    uint64_t npdg;            // 050h
    uint64_t npda;            // 058h
    uint32_t nows;            // 060h
    uint16_t mssrl;           // 064h
    uint32_t mcl;             // 066h
    uint8_t  msrc;            // 06Ah
    uint8_t  nulbaf;          // 06Bh
    uint8_t  _06c[3];         // 06Ch
    uint8_t  kpiodaag;        // 06Fh
    uint8_t  _070[4];         // 070h
    uint32_t anagrpid;        // 074h
    uint8_t  _078[3];         // 078h
    uint8_t  nsattr;          // 07Bh
    uint16_t nvmsetid;        // 07Ch
    uint16_t endgid;          // 07Eh
    uint8_t  nguid[16];       // 080h
    uint8_t  eui64[8];        // 090h
    uint8_t  pad_098[3944];   // 098h-FFFh
};
static_assert(sizeof(identify_ns_nvm_t) == 4096, "identify_ns_nvm_t must be 4096 bytes");

// Identify Controller Data Structure - 4096 bytes
// Only the most commonly accessed fields are defined
struct __attribute__((packed)) identify_ctrl_t {
    uint16_t vid;              // 000h
    uint16_t ssvid;            // 002h
    uint8_t  sn[20];           // 004h
    uint8_t  mn[40];           // 018h
    uint8_t  fr[8];            // 040h
    uint8_t  rab;              // 048h
    uint8_t  ieee[3];          // 049h
    uint8_t  cmic;             // 04Ch
    uint8_t  mdts;             // 04Dh
    uint16_t cntlid;           // 04Eh
    uint32_t ver;              // 050h
    uint32_t rtd3r;            // 054h
    uint32_t rtd3e;            // 058h
    uint32_t oaes;             // 05Ch
    uint32_t ctratt;           // 060h
    uint8_t  rsv_064[28];      // 064h-07Fh
    uint16_t acl;              // 080h
    uint8_t  aerl;             // 082h
    uint8_t  oacs;              // 083h
    uint8_t  rsv_084[380];     // 084h-1FFh
    uint8_t  sqes;             // 200h
    uint8_t  cqes;             // 201h
    uint16_t maxcmd;           // 202h
    uint32_t nn;               // 204h
    uint16_t oncs;             // 208h
    uint16_t fuses;            // 20Ah
    uint8_t  fna;              // 20Ch
    uint8_t  vwc;              // 20Dh
    uint16_t awun;             // 20Eh
    uint16_t awupf;            // 210h
    uint8_t  icsvscc;          // 212h
    uint8_t  nwpc;             // 213h
    uint16_t acwu;             // 214h
    uint16_t cdfs;             // 216h
    uint32_t sgls;             // 218h
    uint32_t mnan;             // 21Ch
    uint8_t  pad_220[3552]; /**/   // 220h-FFFh
};
static_assert(sizeof(identify_ctrl_t) == 4096, "identify_ctrl_t must be 4096 bytes");

// All these structs are 4096 bytes as per NVMe spec
struct __attribute__((packed)) identify_ns_indep_t {
    uint8_t  data[4096];
};
static_assert(sizeof(identify_ns_indep_t) == 4096, "identify_ns_indep_t must be 4096 bytes");

struct __attribute__((packed)) primary_ctrl_caps_t {
    uint8_t  data[4096];
};
static_assert(sizeof(primary_ctrl_caps_t) == 4096, "primary_ctrl_caps_t must be 4096 bytes");

} // namespace NVMe
