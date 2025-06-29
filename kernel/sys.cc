#include "sys.h"
#include "stdint.h"
#include "idt.h"
#include "debug.h"
#include "machine.h"
#include "kernel.h"
#include "ext2.h"
#include "elf.h"
#include "vmm.h"
#include "physmem.h"

void switch_processes(UserContext user_context) {
    // get the current pcb and update its user_context
    PCB* pcb = active_pcbs.mine();
    pcb->page_directory = (uint32_t)(getCR3() & 0xFFFFF000);
    pcb->user_context = user_context;

    // add this pcb back into the event loop so we can come back to it later
    go([pcb]{
        vmm_on(pcb->page_directory);
        active_pcbs.mine() = pcb;
        resume(&pcb->user_context);
    });
    interrupts.mine() = false;

    // get the next pcb through event_loop
    event_loop();
}

void exit(uint32_t value) {
    uint32_t* page_directory = (uint32_t*)(getCR3() & 0xFFFFF000);
    VMM::free(page_directory);
    PCB* pcb = active_pcbs.mine();
    pcb->exit_future.set(value);
    // delete pcb;
    interrupts.mine() = false;
    event_loop();
}

void switch_interrupt(UserContext user_context, uint32_t value) {
    if (interrupts.mine()) {
        // if you experienced an interrupt during a sys call, switch processes and save this
        // return value in the user context 
        interrupts.mine() = false;
        user_context.regs.eax = value;
        switch_processes(user_context);
    }
}

int32_t sigreturn() {
    PCB* pcb = active_pcbs.mine();
    if (!pcb->in_handler) {
        return -1;
    }

    // only resumes if called from within a signal handler
    pcb->in_handler = false;
    switch_processes(pcb->handler_user_context);

    return 1;
}

Node* find_path_node(char* path_name) {
    // must be within user space
    PCB* pcb = active_pcbs.mine();
    if ((uint32_t)(path_name) < 0x80000000 || (uint32_t)(path_name) >= 0xF0000000) {
    // if (!pcb->queue->contains_range((uint32_t)(path_name), (uint32_t)(path_name))) {
        return nullptr;
    }

    Node* current_node = fs->root;
    uint32_t i = 0;
    if (path_name[0] != '/') {
        current_node = pcb->cwd_node;
    }

    // parse the path string
    while (path_name[i] != 0) {
        // the previous node should be a directory
        if (current_node == nullptr || !(current_node->is_dir())) {
            return nullptr;
        }

        // '//' is the same as '/', so ignore extra slashes
        while (path_name[i] == '/') i++;

        // go from i up to the next '/' or NULL
        uint32_t next = i;
        while (path_name[next] != '/' && path_name[next] != 0) next++;

        if (next - i != 0) {
            // split string here, new string is path_name[i, next - 1]
            char* current_name = new char[next - i + 1];
            for (uint32_t j = i; j < next; j++) {
                current_name[j - i] = path_name[j];
            }
            current_name[next - i] = 0;

            current_node = fs->find(current_node, current_name);
            delete[] current_name;
        }
        
        if (path_name[next] == 0) {
            i = next;
            break;
        }
        i = next + 1;
    }
    // must lie within user space
    if ((uint32_t)(path_name) + i - 1 >= 0xF0000000 || (uint32_t)(path_name) + i - 1 < i - 1) {
    // if (!pcb->queue->contains_range((uint32_t)(path_name), (uint32_t)(path_name) + i - 1)  || (uint32_t)(path_name) + i - 1 < i - 1) {
        return nullptr;
    }

    return current_node;
}

int32_t write(uint32_t* userEsp, UserContext user_context) {
    uint32_t fd = userEsp[1];
    char* buffer = (char*)(userEsp[2]);
    size_t count = userEsp[3];
    
    if (count == 0) {
        return 0;
    }

    PCB* pcb = active_pcbs.mine();

    if (fd >= 10 || pcb->file_descriptor[fd].is_null()) {
        return -1;
    }

    // // must be a valid user-accessible virtual address
    if ((uint32_t)(buffer) < 0x80000000 || (uint32_t)(buffer) + count >= 0xF0000000 || (uint32_t)(buffer) + count < count) {
    // if (!pcb->queue->contains_range((uint32_t)buffer, (uint32_t)buffer + count) || (uint32_t)buffer + count < count) {
        return -1;
    }

    Shared<FileDescriptor> file_descriptor = pcb->file_descriptor[fd];
    if (!file_descriptor->writable || (file_descriptor->vnode != nullptr && (file_descriptor->vnode->is_dir() || file_descriptor->vnode->is_file()))) {
        return -1;
    }

    // write up to count bytes
    if (file_descriptor->pipe != nullptr) {
        // writing to pipe
        pcb->page_directory = (uint32_t)(getCR3() & 0xFFFFF000);
        pcb->user_context = user_context;
        file_descriptor->pipe->put(buffer[0], [pcb]{
            vmm_on(pcb->page_directory);
            pcb->user_context.regs.eax = 1;
            active_pcbs.mine() = pcb;
            resume(&pcb->user_context);
        });
        interrupts.mine() = false;
        event_loop();
    }
    else {
        // write to terminal
        for (size_t i = 0; i < count; i++) {
            Debug::printf("%c", buffer[i]);
            // break;
        }

        return count;
    }

    return -1;
}

extern "C" int sysHandler(UserContext user_context) {
    auto userEsp = (uint32_t*) user_context.iFrame.esp;
    auto userEip = user_context.iFrame.eip;
    
    switch(user_context.regs.eax) {
        case 0: {
            // exit()
            exit(userEsp[1]);
        } break;
        case 1:{
            // write()
            return write(userEsp, user_context);
        } break;
        case 2: {
            // fork()
            uint32_t* parent_page_directory = (uint32_t*)(getCR3() & 0xFFFFF000);
            uint32_t child_page_directory = PhysMem::alloc_frame();
            
            // share all page directory indices that are in the kernel space
            for (uint32_t pdi = (0x00001000 >> 22); pdi < (kConfig.memSize >> 22); pdi++) {
                uint32_t pde = parent_page_directory[pdi];
                if ((pde & 1) == 1) {
                    ((uint32_t*) child_page_directory)[pdi] = parent_page_directory[pdi];
                }
            }
                        
            // create deep copies of everything in the user space
            // loop over all page directory indices in the user space
            for (uint32_t pdi = (0x80000000 >> 22); pdi < (0xF0000000 >> 22); pdi++) {
                uint32_t pde = parent_page_directory[pdi];
                if ((pde & 1) == 1) {
                    // this is a page fault, allocate a table for this child pd
                    ((uint32_t*)child_page_directory)[pdi] = PhysMem::alloc_frame() | 0x7;

                    uint32_t* parent_page_table = (uint32_t*)(parent_page_directory[pdi] & 0xFFFFF000);
                    uint32_t* child_page_table = (uint32_t*)(((uint32_t*)child_page_directory)[pdi] & 0xFFFFF000);
                    // loop over all page table indices in the page table that page directory points to
                    for (uint32_t pti = 0; pti < 1024; pti++) {

                        uint32_t pte = parent_page_table[pti];
                        if ((pte & 1) == 1) {
                            // this is a page fault, allocate a data frame for this child pt
                            child_page_table[pti] = PhysMem::alloc_frame() | 0x107;

                            uint32_t* parent_data_frame = (uint32_t*)(parent_page_table[pti] & 0xFFFFF000);
                            uint32_t* child_data_frame = (uint32_t*)(child_page_table[pti] & 0xFFFFF000);
                            // loop over all data frame indices in the data frame that page table points to
                            for (uint32_t data_index = 0; data_index < 1024; data_index++) {
                                uint32_t data_entry = parent_data_frame[data_index];
                                // copy over the data entry into the child data frame
                                child_data_frame[data_index] = data_entry;
                            }
                        }
                    }
                }
            }

            // set page directory to map to the same apic tables as the parent page directory
            ((uint32_t*)child_page_directory)[kConfig.ioAPIC >> 22] = parent_page_directory[kConfig.ioAPIC >> 22];
            // set page directory to map to the shared page table as the parent page directory
            ((uint32_t*)child_page_directory)[0xF0000000 >> 22] = parent_page_directory[0xF0000000 >> 22];
            
            // create a new child pcb and add it as a child to the current pcb
            PCB* child_pcb = new PCB(child_page_directory);
            PCB* current_pcb = active_pcbs.mine();
            current_pcb->add_child(child_pcb);

            // children inherit semaphores from their parents
            for (uint32_t i = 0; i < 100; i++) {
                child_pcb->semaphores[i] = current_pcb->semaphores[i];
            }

            // deep copy over VMEs to child
            child_pcb->queue = current_pcb->queue->deep_copy();

            // copy over handler information to child
            child_pcb->handler_eip = current_pcb->handler_eip;
            child_pcb->in_handler = current_pcb->in_handler;
            child_pcb->handler_user_context = current_pcb->handler_user_context;

            // copy over the file descriptor
            // child_pcb->file_descriptor = current_pcb->copy_file_descriptor();
            for (uint32_t i = 0; i < 10; i++) {
                child_pcb->file_descriptor[i] = current_pcb->file_descriptor[i];
            }
            child_pcb->cwd_node = current_pcb->cwd_node;

            go([child_page_directory, child_pcb, userEsp, userEip] {
                vmm_on(child_page_directory);
                active_pcbs.mine() = child_pcb;
                switchToUser(userEip, (uint32_t)userEsp, 0);
            });

            return 1;
        } break;
        case 7: {
            // shutdown()
            uint32_t* page_directory = (uint32_t*)(getCR3() & 0xFFFFF000);
            VMM::free(page_directory);
            delete active_pcbs.mine();
            Debug::shutdown();
        } break;
        case 998: {
            // yield()
            switch_processes(user_context);
        } break;
        case 999: {
            // join()
            PCB* pcb = active_pcbs.mine();
            pcb->user_context = user_context;
            if (pcb->is_empty()) {
                // this pcb has no child
                return -1;
            }

            PCB* child = pcb->peek_child();
            if (child != nullptr) {
                child->exit_future.get([pcb] (auto value) {
                    pcb->remove_child();
                    pcb->user_context.regs.eax = value;
                    vmm_on(pcb->page_directory);
                    active_pcbs.mine() = pcb;
                    resume(&pcb->user_context);
                });
            }
            interrupts.mine() = false;
            event_loop();
        } break;
        case 1000: {
            // execl()
            char* path_name = (char*)userEsp[1];

            Node* current_node = find_path_node(path_name);
            if (current_node == nullptr || !(current_node->is_file())) {
                return -1;
            }

            // find number of arguments
            uint32_t count_args = 0;
            while (userEsp[count_args + 2] != 0) {
                count_args++;
            }

            // PCB* pcb = active_pcbs.mine();

            // parse all the arguments
            char** args = new char*[count_args];
            uint32_t* string_lengths = new uint32_t[count_args];
            uint32_t* string_pointers = new uint32_t[count_args];
            int32_t total_length = 0;
            for (uint32_t j = 0; j < count_args; j++) {
                char* arg = (char*)userEsp[j + 2];

                // find length of string
                uint32_t string_length = 0;
                while (arg[string_length] != 0) {
                    string_length++;
                }
                string_length++;
                total_length += string_length;

                // arguments must lie within user space
                if ((uint32_t)(arg) < 0x80000000 || (uint32_t)(arg) + string_length > 0xF0000000 || (uint32_t)(arg) + string_length < string_length) {
                // if (!pcb->queue->contains_range((uint32_t)(arg), (uint32_t)(arg) + string_length)) {
                    delete[] string_lengths;
                    delete[] string_pointers;
                    for (uint32_t j = 0; j < count_args; j++) {
                        delete[] args[j];
                    }
                    delete[] args;
                    return -1;
                }

                // push the arguments onto the current heap
                args[j] = new char[string_length];
                memcpy(args[j], arg, string_length);
                string_lengths[j] = string_length;
                string_pointers[j] = (uint32_t)((char*)userEsp - total_length);
            }
            
            int32_t e = ELF::valid_load(current_node);
            
            // check if the current_node is a valid ELF file
            if (e == -1) {
                delete[] string_lengths;
                delete[] string_pointers;
                for (uint32_t j = 0; j < count_args; j++) {
                    delete[] args[j];
                }
                delete[] args;
                return -1;
            }

            uint32_t* page_directory = (uint32_t*)(getCR3() & 0xFFFFF000);
            VMM::free(page_directory);
            VMM::per_core_init();
            uint32_t new_page_directory = (getCR3() & 0xFFFFF000);
            active_pcbs.mine()->page_directory = new_page_directory;
            e = ELF::load(current_node);

            // push the previous arguments from the current heap onto the user stack
            for (uint32_t j = 0; j < count_args; j++) {
                memcpy((char*)string_pointers[j], args[j], string_lengths[j]);
                delete[] args[j];
            }
            delete[] args;
            delete[] string_lengths;

            // align total length
            total_length = -((total_length + 3) >> 2);

            // add nullptr between strings and the string pointers
            total_length--;
            userEsp[total_length] = 0;

            // add pointers to all strings
            for (int32_t j = count_args - 1; j >= 0; j--) {
                total_length--;
                userEsp[total_length] = string_pointers[j];
            }
            delete[] string_pointers;

            total_length--;
            userEsp[total_length] = (uint32_t)userEsp + ((total_length + 1) << 2);
            total_length--;
            userEsp[total_length] = count_args;

            userEsp += total_length;
            
            if (interrupts.mine()) {
                interrupts.mine() = false;
                PCB* pcb = active_pcbs.mine();
                pcb->user_context = user_context;
                go([e, userEsp, pcb, new_page_directory] {
                    pcb->page_directory = new_page_directory;
                    vmm_on(pcb->page_directory);
                    active_pcbs.mine() = pcb;
                    switchToUser(e, (uint32_t)userEsp, 0);
                });
                event_loop();
            }
            else {
                switchToUser(e, (uint32_t)userEsp, 0);
            }
        } break;
        case 1001: {
            // sem()
            PCB* pcb = active_pcbs.mine();
            uint32_t n = userEsp[1];
            for (uint32_t i = 0; i < 100; i++) {
                if (pcb->semaphores[i].is_null()) {
                    pcb->semaphores[i] = Shared<Semaphore>::make(n);
                    return i;
                }
            }

            // 100 semaphores already created, cannot create any more.
            return -1;
        } break;
        case 1002: {
            // up()
            PCB* pcb = active_pcbs.mine();
            uint32_t i = userEsp[1];
            if (i < 0 || i >= 100 || pcb->semaphores[i].is_null()) {
                // this does not point to a valid semaphore, return -1
                return -1;
            }
            Shared<Semaphore> semaphore = pcb->semaphores[i];
            semaphore->up();
            return 0;
        } break;
        case 1003: {
            // down()
            // switch processes using semaphore
            PCB* pcb = active_pcbs.mine();
            uint32_t i = userEsp[1];
            if (i < 0 || i >= 100 || pcb->semaphores[i].is_null()) {
                // this does not point to a valid semaphore, return -1
                return -1;
            }
            Shared<Semaphore> semaphore = pcb->semaphores[i];
            pcb->user_context = user_context;
            semaphore->down([pcb]{
                vmm_on(pcb->page_directory);
                pcb->user_context.regs.eax = 0;
                active_pcbs.mine() = pcb;
                resume(&pcb->user_context);
            });
            interrupts.mine() = false;
            event_loop();
        } break;
        case 1004: {
            // simple_signal()
            PCB* pcb = active_pcbs.mine();
            pcb->handler_eip = userEsp[1];
        } break;
        case 1005: {
            // simple_mmap()
            PCB* pcb = active_pcbs.mine();
            uint32_t addr = userEsp[1];
            uint32_t size = userEsp[2];
            int32_t fd = userEsp[3];
            uint32_t offset = userEsp[4];

            if (size == 0) {
                return 0;
            }

            if ((addr & 0xFFF) != 0) {
                // address is not page aligned
                return 0;
            }
            if ((size & 0xFFF) != 0) {
                // size is not page aligned
                return 0;
            }
            if (addr != 0 && (addr < 0x80000000 || addr + size >= 0xF0000000 || addr + size < addr)) {
                // the desired region is not fully accessible in user mode
                return 0;
            }
            if (addr != 0 && pcb->queue->intersects_queue(addr, size)) {
                // the desired region intersects with an existing mapping
                return 0;
            }
            
            Node* node = nullptr;
            if (fd != -1) {
                if ((offset & 0xFFF) != 0) {
                    // offset is not page aligned
                    return 0;
                }

                if (fd >= 10 || pcb->file_descriptor[fd].is_null()) {
                    // fd does not refer to an valid file descriptor
                    return 0;
                }

                node = pcb->file_descriptor[fd]->vnode;
                if (node == nullptr || node->is_dir()) {
                    return 0;
                }
            }

            // add the vme
            uint32_t va = pcb->queue->add_vme(new VME(addr, size));
                
            if (fd == -1) {
                return va;
            }

            // read_size = min(size, (len(file)-offset))
            uint32_t read_size = node->size_in_bytes() - offset;
            if (size < read_size) read_size = size;
            
            int32_t n = node->read_all(offset, read_size, (char*)va);

            // zero fill the rest
            for (uint32_t i = va + n; i < va + size; i++) {
                *((char*)i) = 0;
            }

            return va;
        } break;
        case 1006: {
            // sigreturn()
            return sigreturn();
        } break;
        case 1007: {
            // sem_close()
            PCB* pcb = active_pcbs.mine();
            uint32_t i = userEsp[1];
            if (i < 0 || i >= 100 || pcb->semaphores[i].is_null()) {
                // this does not point to a valid semaphore, return -1
                return -1;
            }
            pcb->semaphores[i] = {};
            return 0;
        } break;
        case 1008: {
            // simple_munmap()
            PCB* pcb = active_pcbs.mine();
            uint32_t addr = userEsp[1];
            if (addr < 0x80000000 || addr >= 0xF0000000) {
            // if (!pcb->queue->contains_range(addr, addr)) {
                // addr is outside the process private range
                return -1;
            }
            return pcb->remove_from_vmequeue(addr) ? 0 : -1;
        } break;
        case 1020: {
            // chdir()
            char* path_name = (char*)userEsp[1];
            Node* current_node = find_path_node(path_name);
            if (current_node == nullptr) {
                return -1;
            }

            PCB* pcb = active_pcbs.mine();
            pcb->cwd_node = current_node;
            return 0;
        } break;
        case 1021: {
            // open()
            char* path_name = (char*)userEsp[1];
            Node* current_node = find_path_node(path_name);
            if (current_node == nullptr) {
                return -1;
            }

            PCB* pcb = active_pcbs.mine();
            for (uint32_t i = 0; i < 10; i++) {
                if (pcb->file_descriptor[i].is_null()) {
                    // this is a valid location for the current node
                    pcb->file_descriptor[i] = Shared<FileDescriptor>::make(current_node, new Atomic<uint32_t>(0));
                    return i;
                }
            }

            // no available file descriptors
            return -1;
        } break;
        case 1022: {
            // close()
            uint32_t fd = userEsp[1];
            PCB* pcb = active_pcbs.mine();
            if (fd >= 10 || pcb->file_descriptor[fd].is_null()) {
                return -1;
            }

            pcb->file_descriptor[fd] = {};
            return 0;
        } break;
        case 1023: {
            // len()
            uint32_t fd = userEsp[1];
            PCB* pcb = active_pcbs.mine();
            if (fd >= 10 || pcb->file_descriptor[fd].is_null() || pcb->file_descriptor[fd]->pipe != nullptr) {
                return -1;
            }

            Node* node = pcb->file_descriptor[fd]->vnode;
            if (!(node->is_file())) {
                return -1;
            }

            return node->size_in_bytes();
        } break;
        case 1024: {
            // read()
            uint32_t fd = userEsp[1];
            char* buffer = (char*)(userEsp[2]);
            size_t count = userEsp[3];

            if (count == 0) {
                return 0;
            }

            PCB* pcb = active_pcbs.mine();

            if (fd >= 10 || pcb->file_descriptor[fd].is_null()) {
                return -1;
            }

            // must be a valid user-accessible virtual address
            if ((uint32_t)buffer < 0x80000000 || (uint32_t)buffer + count >= 0xF0000000 || (uint32_t)buffer + count < count) {
            // if (!pcb->queue->contains_range((uint32_t)buffer, (uint32_t)buffer + count)) {
                return -1;
            }

            Shared<FileDescriptor> file_descriptor = pcb->file_descriptor[fd];
            if (!file_descriptor->readable || (file_descriptor->vnode != nullptr && file_descriptor->vnode->is_dir())) {
                return -1;
            }

            // read up to count bytes 
            if (file_descriptor->pipe != nullptr) {
                pcb->page_directory = (uint32_t)(getCR3() & 0xFFFFF000);
                pcb->user_context = user_context;
                file_descriptor->pipe->get([pcb, buffer] (char value) {
                    vmm_on(pcb->page_directory);
                    pcb->user_context.regs.eax = 1;
                    buffer[0] = value;
                    active_pcbs.mine() = pcb;
                    resume(&pcb->user_context);
                });
                interrupts.mine() = false;
                event_loop();
            }
            else {
                Node* node = file_descriptor->vnode;
                if (count > node->size_in_bytes() - file_descriptor->offset->get()) {
                    count = node->size_in_bytes() - file_descriptor->offset->get();
                }
                int32_t n = file_descriptor->vnode->read_all(file_descriptor->offset->fetch_add(count), count, buffer);
                // int32_t n = file_descriptor->vnode->read_all(file_descriptor->offset->fetch_add(1), 1, buffer);
                return n == -1 ? 0 : n;
            }
        } break;
        case 1025: {
            // write()
            return write(userEsp, user_context);
        } break;
        case 1026: {
            // pipe()
            uint32_t* write_fd = (uint32_t*)userEsp[1];
            uint32_t* read_fd = (uint32_t*)userEsp[2]; 

            PCB* pcb = active_pcbs.mine();

            uint32_t count = 0;
            for (uint32_t i = 0; i < 10; i++) {
                if (pcb->file_descriptor[i].is_null()) {
                    count++;
                }
            }
            if (count < 2) {
                return -1;
            }

            BoundedBuffer<char>* pipe = new BoundedBuffer<char>(100);
            *write_fd = 20;
            *read_fd = 20;
            for (uint32_t i = 0; i < 10; i++) {
                if (pcb->file_descriptor[i].is_null()) {
                    if ((*write_fd) == 20) {
                        *write_fd = i;
                        pcb->file_descriptor[i] = Shared<FileDescriptor>::make(false, true, pipe);
                    }
                    else {
                        *read_fd = i;
                        pcb->file_descriptor[i] = Shared<FileDescriptor>::make(true, false, pipe);
                        return 0;
                    }
                }
            }

            return -1;
        } break;
        case 1027: {
            // kill()
            uint32_t v = userEsp[1];
            PCB* pcb = active_pcbs.mine();
            PCB* child = pcb->peek_child();
            if (child == nullptr || child->killed || child->handled) {
                return -1;
            }

            child->killed = true;
            child->killed_v = v;

            return 0;
        } break;
        case 1028: {
            // dup()
            uint32_t fd = userEsp[1];
            PCB* pcb = active_pcbs.mine();
            if (fd >= 10 || pcb->file_descriptor[fd].is_null()) {
                return -1;
            }

            for (uint32_t i = 0; i < 10; i++) {
                if (pcb->file_descriptor[i].is_null()) {
                    // this is a valid location to dupe
                    pcb->file_descriptor[i] = pcb->file_descriptor[fd];
                    return i;
                }
            }

            return -1;
        } break;
        default:
            Debug::panic("syscall %d (%x, %x, %x)\n",user_context.regs.eax, userEsp[0], userEsp[1], userEsp[2]);
    }

    return 1;
}

extern "C" int sysHandlerWrap(UserContext user_context) {
    int value = sysHandler(user_context);
    switch_interrupt(user_context, value);
    return value;
}

void SYS::init(void) {
    IDT::trap(48,(uint32_t)sysHandler_,3);
}
