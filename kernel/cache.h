#ifndef _CACHE_H_
#define _CACHE_H_

#include "stdint.h"
#include "block_io.h"
#include "atomic.h"
#include "queue.h"
#include "ide.h"

class Cache : public BlockIO {  // We are a block device

    Ide* ide;
    uint32_t block_size;
    MRUQueue mru{block_size};

    SpinLock lock;

public:
    Cache(Ide* ide, uint32_t block_size) : BlockIO(block_size), ide(ide), block_size(block_size), lock() {}

    virtual ~Cache() {}
    
    // Read the given block into the given buffer. We assume the
    // buffer is big enough
    void read_block(uint32_t block_number, char* buffer) override;

    // We lie because I'm too lazy to get the actual drive size
    // This means that we'll get QEMU errors if we try to access
    // non existent blocks.
    uint32_t size_in_bytes() {
        // This is not correct
        return ~(uint32_t(0));
    }

};

#endif