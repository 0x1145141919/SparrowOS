#pragma once
#include "stdint.h" 
/* === Runtime environment detection === */
enum runtime_env : uint8_t {
    ENV_BARE_METAL = 0,
    ENV_KVM        = 1,
    ENV_TCG        = 2,
};

extern runtime_env g_env;
runtime_env probe_env(void);