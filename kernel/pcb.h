#ifndef _PCB_H_
#define _PCB_H_

#include "stdint.h"
#include "future.h"
#include "physmem.h"
#include "ext2.h"
#include "bb.h"

extern Ext2* fs;

struct Registers {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t scratch;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
};

struct IFrame {
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    uint32_t esp;
    uint32_t ss;
};

struct UserContext {
    Registers regs;
    IFrame iFrame;
};

extern void switch_processes(UserContext user_context);

struct FileDescriptor {
    Node* vnode = nullptr;
    Atomic<uint32_t>* offset;
    bool readable, writable;
    BoundedBuffer<char>* pipe = nullptr;

    FileDescriptor(Node* node, Atomic<uint32_t>* offset) : vnode(node), offset(offset), 
        readable(true), writable(false) {}
    
    FileDescriptor(bool readable, bool writable) : readable(readable), writable(writable) {}

    FileDescriptor(bool readable, bool writable, BoundedBuffer<char>* pipe) : readable(readable), writable(writable), pipe(pipe) {}
};

class PCB {
public:
    uint32_t page_directory;
    Future<uint32_t> exit_future;
    UserContext user_context;

    uint32_t size, capacity;
    PCB** children;

    Shared<Semaphore>* semaphores;

    bool in_handler;
    uint32_t handler_eip;
    UserContext handler_user_context;

    VMEQueue* queue;

    Shared<FileDescriptor>* file_descriptor;
    Node* cwd_node;                            // current working directory node

    bool killed;
    uint32_t killed_v;
    bool handled;

    PCB(uint32_t page_directory) : page_directory(page_directory), exit_future(), size(0), 
        capacity(1), children(new PCB*[1]), semaphores(new Shared<Semaphore>[100]),
        in_handler(false), handler_eip(0), queue(new VMEQueue()), 
        file_descriptor(new Shared<FileDescriptor>[10]), cwd_node(), killed(false), killed_v(0),
        handled(false) {
            // add user stack onto vme
            queue->add_vme(new VME(0xF0000000 - 0x100000, 0x100000));
        }

    ~PCB() {
        delete[] children;
        delete queue;
    }

    bool remove_from_vmequeue(uint32_t addr) {
        VME* removed_vme = queue->remove(addr);
        if (removed_vme == nullptr) {
            return false;
        }        
        // free all pages in removed_vme
        for (uint32_t pdi = (removed_vme->start >> 22); pdi <= (removed_vme->end >> 22); pdi++) {
            uint32_t pde = ((uint32_t*)page_directory)[pdi];
            if ((pde & 1) == 1) {
                uint32_t* page_table = (uint32_t*)(pde & 0xFFFFF000);
                // loop over all page table indices in the page table that page directory points to
                uint32_t start = 0;
                uint32_t end = 1024;
                if (pdi == (removed_vme->start >> 22)) {
                    start = (removed_vme->start >> 12) & 0x3FF; 
                }
                if (pdi == (removed_vme->end >> 22)) {
                    end = (removed_vme->end >> 12) & 0x3FF;
                }
                for (uint32_t pti = start; pti < end; pti++) {
                    uint32_t pte = page_table[pti];
                    if ((pte & 1) == 1) {
                        uint32_t data_frame = page_table[pti] & 0xFFFFF000;
                        PhysMem::dealloc_frame(data_frame);
                        page_table[pti] = 0;
                    }
                }
                if (start == 0 && end == 1024) {
                    PhysMem::dealloc_frame((uint32_t)(page_table));
                    ((uint32_t*)page_directory)[pdi] = 0;
                }
            }
        }

        return true;
    }

    void resize() {
        PCB** new_children = new PCB*[capacity << 1];
        for (uint32_t i = 0; i < capacity; i++) {
            new_children[i] = children[i];
        }
        delete[] children;

        capacity <<= 1;
        children = new_children;
    }

    bool is_empty() {
        return size == 0;
    }

    void add_child(PCB* child) {
        if (size == capacity) {
            resize();
        }
        children[size] = child;
        size++;
    }

    PCB* peek_child() {
        if (size == 0) return nullptr;
        return children[size - 1];
    }

    PCB* remove_child() {
        size--;
        return children[size];
    }

    void init_file_descriptor() {
        file_descriptor[0] = Shared<FileDescriptor>::make(false, false);
        file_descriptor[1] = Shared<FileDescriptor>::make(false, true);
        file_descriptor[2] = Shared<FileDescriptor>::make(false, true);
        cwd_node = fs->root;
    }
};

#endif