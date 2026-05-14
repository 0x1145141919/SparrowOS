#include "panic.h"
#include <cstdio>
#include <cstdlib>
void Panic::panic(panic_behaviors_flags, char* msg, panic_context::x64_context*, panic_info_inshort*, KURD_t) {
    fprintf(stderr, "PANIC: %s\n", msg ? msg : "(no message)");
    exit(1);
}
