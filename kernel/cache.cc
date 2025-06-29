#include "cache.h"
#include "ide.h"
#include "stdint.h"
#include "debug.h"
#include "machine.h"
#include "threads.h"
#include "atomic.h"
#include "smp.h"

void Cache::read_block(uint32_t block_id, char* buffer) {
    LockGuard g{lock};
    
    if (mru.contains(block_id)) {
        // if the node is already cached, then just get the buffer and return it
        MRUNode* cached_node = mru.peek();
        for (uint32_t i = 0; i < block_size; i++) {
            buffer[i] = cached_node->buffer[i];
        }
    }
    else {
        // read, then store in the MRU
        ide->read_all(block_id * block_size, block_size, buffer);
        mru.add(block_id, buffer);
    }
}