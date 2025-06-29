#include "vmm.h"
#include "machine.h"
#include "idt.h"
#include "libk.h"
#include "config.h"
#include "debug.h"
#include "physmem.h"
#include "sys.h"
#include "elf.h"
#include "events.h"
#include "kernel.h"

unsigned int volatile* fault = (unsigned int volatile*) 0xF0000800;

namespace VMM {

uint32_t the_shared_frame;
uint32_t page_directory;
uint32_t kernel_pt[32];
uint32_t apic_pt;
uint32_t shared_pt;

void global_init() {
    // initialize the shared page table
    shared_pt = PhysMem::alloc_frame();

    // initialize the shared frame
    the_shared_frame = PhysMem::alloc_frame();

    ((uint32_t*)shared_pt)[0] = the_shared_frame | 0x107;

    //  Advanced Programmable Interrupt Controllers (IOPIC and LAPIC)
    apic_pt = PhysMem::alloc_frame();
    // set the middle 10 digits
    ((uint32_t*)apic_pt)[(kConfig.localAPIC >> 12) & 0x3FF] = kConfig.localAPIC | 0x113;
    ((uint32_t*)apic_pt)[(kConfig.ioAPIC >> 12) & 0x3FF] = kConfig.ioAPIC | 0x113;

    // set up identity mapping in the kernel page table
    for (uint32_t i = 0; i < 32; i++) {
        kernel_pt[i] = PhysMem::alloc_frame();
        for (uint32_t j = 0; j < 1024; j++) {
            if (i == 0 && j == 0) {
                continue;
            }
            ((uint32_t*)kernel_pt[i])[j] = (i << 22) | (j << 12) | 0x103;
        }
    }
}

void per_core_init() {
    page_directory = PhysMem::alloc_frame();
    // set page directory to map to the kernel page tables
    for (uint32_t i = 0; i < 32; i++) {
        ((uint32_t*)page_directory)[i] = kernel_pt[i] | 0x3;
    }
    // set page directory to map to the apic tables
    ((uint32_t*)page_directory)[kConfig.ioAPIC >> 22] = apic_pt | 0x13;
        // set page directory to map to the shared page table
    ((uint32_t*)page_directory)[0xF0000000 >> 22] = shared_pt | 0x7;
    
    vmm_on(page_directory);
}

void free(uint32_t* page_directory) {
    // loop over all page directory indices in the user space, and free them
    for (uint32_t pdi = (0x80000000 >> 22); pdi < (0xF0000000 >> 22); pdi++) {
        uint32_t pde = page_directory[pdi];
        if ((pde & 1) == 1) {
            uint32_t* page_table = (uint32_t*)(page_directory[pdi] & 0xFFFFF000);
            // loop over all page table indices in the page table that page directory points to
            for (uint32_t pti = 0; pti < 1024; pti++) {
                uint32_t pte = page_table[pti];
                if ((pte & 1) == 1) {
                    uint32_t data_frame = page_table[pti] & 0xFFFFF000;
                    PhysMem::dealloc_frame(data_frame);
                    page_table[pti] = 0;
                }
            }
            PhysMem::dealloc_frame((uint32_t)(page_table));
            ((uint32_t*)page_directory)[pdi] = 0;
        }
    }
}

} /* namespace vmm */

extern "C" void vmm_pageFault(uintptr_t va_, Registers regs, uint32_t error, IFrame iFrame) {
    PCB* pcb = active_pcbs.mine();
    if (pcb->in_handler && va_ == 0x2000) {
        // returned without explicitly calling sigreturn. returns needs to behave as if it called sigreturn
        sigreturn();
    }
    else if (!pcb->queue->contains_range(va_, va_)) {
        // if you page fault inside the handler function, or there is no
        // registered signal handler then you should exit with code 139
        if (pcb->handler_eip == 0 || pcb->in_handler) {
            *fault = va_;
            exit(139);
        }
        
        // call the handler to handle segmentation violation exception
        UserContext user_context{regs, iFrame};
        pcb->handler_user_context = user_context;
        pcb->in_handler = true;
        uint32_t esp = iFrame.esp;
        *(uint32_t*)(esp - 128 - 4) = va_;
        *(uint32_t*)(esp - 128 - 8) = 1;
        *(uint32_t*)(esp - 128 - 12) = 0x2000;
        switchToUser(pcb->handler_eip, esp - 128 - 12, 0);
    }
    else {
        // get the page directory from the cr3 register
        uint32_t* page_directory = (uint32_t*)(getCR3() & 0xFFFFF000);
        // get the page directory index from the top 10 bits
        uint32_t pdi = (va_ >> 22);
        // get the page directory entry from the page directory at that index
        uint32_t pde = page_directory[pdi];
        if ((pde & 1) == 0) {
            // this is a page fault, allocate a table for this va
            page_directory[pdi] = PhysMem::alloc_frame() | 0x7;
        }

        // get the page table from the previous ppn
        uint32_t* page_table = (uint32_t*)(page_directory[pdi] & 0xFFFFF000);
        // get the page table index from the middle 10 bits
        uint32_t pti = (va_ >> 12) & 0x3FF;
        // get the page table entry from the page table at that index
        uint32_t pte = page_table[pti];
        if ((pte & 1) == 0) {
            // this is a page fault, allocate a data frame for this va
            page_table[pti] = PhysMem::alloc_frame() | 0x107;
        }
    }
}