#ifndef _queue_h_
#define _queue_h_

#include "atomic.h"
#include "debug.h"
#include "physmem.h"

template <typename T, typename LockType>
class Queue {
    T * volatile first = nullptr;
    T * volatile last = nullptr;
    uint32_t size;
    LockType lock;
public:
    Queue() : first(nullptr), last(nullptr), size(0), lock() {}
    Queue(const Queue&) = delete;
    Queue& operator=(Queue&) = delete;

    void monitor_add() {
        monitor((uintptr_t)&last);
    }

    void monitor_remove() {
        monitor((uintptr_t)&first);
    }

    void add(T* t) {
        LockGuard g{lock};
        t->next = nullptr;
        if (first == nullptr) {
            first = t;
        } else {
            last->next = t;
        }
        last = t;
    }

    T* remove() {
        LockGuard g{lock};
        if (first == nullptr) {
            return nullptr;
        }
        auto it = first;
        first = it->next;
        if (first == nullptr) {
            last = nullptr;
        }
        return it;
    }

    T* remove_all() {
        LockGuard g{lock};
        auto it = first;
        first = nullptr;
        last = nullptr;
        return it;
    }

    void clear() {
        LockGuard g{lock};
        T* current = remove();
        while (current != nullptr) {
            delete current;
            current = remove();
        }
        delete first;
        delete last;
    }
};

struct MRUNode{
    MRUNode* next = nullptr;
    MRUNode* prev = nullptr;
    uint32_t id;
    char* buffer;

    MRUNode(uint32_t id, char* buffer) : id(id), buffer(buffer) {}
};

class MRUQueue {
    MRUNode * volatile first = nullptr;
    MRUNode * volatile last = nullptr;
    uint32_t block_size;
public:
    MRUQueue(uint32_t block_size) : first(nullptr), last(nullptr), block_size(block_size) {
        // fixed size LRU
        first = new MRUNode(-1, (char*)(new uint32_t[block_size >> 2]));
        auto it = first;
        for (uint32_t i = 0; i < 14; i++) {
            MRUNode* cur = new MRUNode(-1, (char*)(new uint32_t[block_size >> 2]));
            it->next = cur;
            cur->prev = it;
            it = cur;
        }
        last = new MRUNode(-1, (char*)(new uint32_t[block_size >> 2]));
        last->prev = it;
        it->next = last;
    }

    MRUQueue(const MRUQueue&) = delete;
    MRUQueue& operator=(MRUQueue&) = delete;
    ~MRUQueue() {
        clear();
    }

    void monitor_add() {
        monitor((uintptr_t)&last);
    }

    void monitor_remove() {
        monitor((uintptr_t)&first);
    }

    void add(uint32_t index, char* buffer) {
        // if (contains(index)) return;
        // move last element to the front and then update it with this new index & buffer
        auto cur = last;
        last = last->prev;
        last->next = nullptr;

        cur->prev = nullptr;
        cur->next = first;
        first->prev = cur;
        first = cur;

        cur->id = index;
        for (uint32_t i = 0; i < block_size; i++) {
            first->buffer[i] = buffer[i];
        }
    }

    MRUNode* peek() {
        return first;
    }

    bool contains(uint32_t id) {
        auto it = first;
        while (it != nullptr) {
            if (it->id == id) {
                // found the correct id, move to the front of the queue and return true
                if (it->prev != nullptr) {
                    it->prev->next = it->next;
                }
                else {
                    return true;
                }
                if (it->next != nullptr) {
                    it->next->prev = it->prev;
                }
                else {
                    last = it->prev;
                }

                it->prev = nullptr;
                it->next = first;
                first->prev = it;
                first = it;
                return true;
            }

            it = it->next;
        }
        return false;
    }

    bool is_empty() {
        return first == nullptr;
    }

    void clear() {
        while (!is_empty()) {
            auto it = first;
            first = it->next;
            delete[] it->buffer;
            delete it;
            if (first == nullptr) {
                last = nullptr;
            }
            else {
                first->prev = nullptr;
            }
        }
    }
};

struct VME {
    VME* next = nullptr;
    VME* prev = nullptr;
    uint32_t start, end, size;

    VME(uint32_t addr, uint32_t size) : start(addr), end(addr + size), size(size) {}
};

class VMEQueue {
    VME* first = nullptr;
    VME* last = nullptr;
    uint32_t size;
public:
    VMEQueue() : first(nullptr), last(nullptr), size(0) {}
    VMEQueue(const VMEQueue&) = delete;
    VMEQueue& operator=(VMEQueue&) = delete;
    ~VMEQueue() {
        clear();
    }

    void monitor_add() {
        monitor((uintptr_t)&last);
    }

    void monitor_remove() {
        monitor((uintptr_t)&first);
    }

    bool intersects_queue(uint32_t addr, uint32_t size) {
        auto it = first;
        while (it != nullptr) {
            if (addr < it->end && addr + size >= it->start) {
                return true;
            }
            it = it->next;
        }
        return false;
    }

    uint32_t add_vme(VME* vme) {
        if (vme->size == 0) {
            return 0;
        }
        VME* prev = nullptr;
        VME* next = first;

        if (vme->start == 0) {
            // add this vme to the start address
            uint32_t start_address = 0x80000000;
            // continue through first-fit search to find the best fit
            while (next != nullptr && next->start < start_address + vme->size) {
                start_address = next->end;
                prev = next;
                next = next->next;
            }
            if (start_address + vme->size > 0xF0000000 || start_address + vme->size < start_address) {
                // the desired region is not fully accessible in user mode
                return 0;
            }

            // otherwise, malloc it by inserting it into this queue
            vme->start = start_address;
            vme->end = start_address + vme->size;
        }
        else {
            // find the location of in this queue that this vme would go
            while (next != nullptr && next->start < vme->end) {
                prev = next;
                next = next->next;
            }
        }

        vme->prev = prev;
        vme->next = next;
        if (prev != nullptr) {
            prev->next = vme;
        }
        else {
            first = vme;
        }
        if (next != nullptr) {
            next->prev = vme;
        }
        else {
            last = vme;
        }

        return vme->start;
    }

    VME* remove(uint32_t addr) {
        auto it = first;
        while (it != nullptr) {
            if (addr < it->end && addr >= it->start) {
                // remove this node for this address
                if (it->prev != nullptr) {
                    it->prev->next = it->next;
                }
                else {
                    first = it->next;
                }
                if (it->next != nullptr) {
                    it->next->prev = it->prev;
                }
                else {
                    last = it->prev;
                }

                return it;
            }
            it = it->next;
        }
        return nullptr;
    }

    VMEQueue* deep_copy() {
        VMEQueue* vme_queue = new VMEQueue();
        auto it = last;
        while (it != nullptr) {
            vme_queue->add_vme(new VME(it->start, it->size));
            it = it->prev;
        }
        return vme_queue;
    }

        // check if the whole range [start, end] lies within the vmes
    bool contains_range(uint32_t start, uint32_t end) {
        auto it = first;
        while (it != nullptr) {
            // you're in this vme range
            if (start >= it->start && start < it->end) {
                if (end < it->end) {
                    return true;
                }
                else {
                    // continue to see if you can build the range
                    start = it->end;
                }
            }
            it = it->next;
        }
        return false;
    }

    bool is_empty() {
        return first == nullptr;
    }

    void clear() {
        while (!is_empty()) {
            auto it = first;
            first = it->next;
            delete it;
            if (first == nullptr) {
                last = nullptr;
            }
            else {
                first->prev = nullptr;
            }
        }
    }
};

#endif
