#ifndef _VMM_H_
#define _VMM_H_

#include "stdint.h"

namespace VMM {

    // Called (on the initial core) to initialize data structures, etc
    extern void global_init();

    // Called on each core to do per-core initialization
    extern void per_core_init();

    extern void free(uint32_t* page_directory);
}

#endif
