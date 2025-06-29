#include "elf.h"
#include "machine.h"
#include "debug.h"
#include "config.h"
#include "kernel.h"

uint32_t ELF::valid_load(Node* file) {
    ElfHeader header;
    uint32_t file_size = file->size_in_bytes();
    if (file_size < sizeof(header)) {
        return -1;
    }
    file->read(0, header);

    // check if this file is an elf file
    if (header.magic0 != 0x7f || header.magic1 != 'E' || header.magic2 != 'L' || header.magic3 != 'F' || header.cls != 1 || header.machine != 3 || header.version != 1) {
        return -1;
    }

    // check if the entry point is outside the user range
    if (header.entry < 0x80000000 || header.entry >= 0xF0001000) {
        return -1;
    }

    if (header.phoff > file_size || file_size < header.phnum * header.phentsize) {
        return -1;
    }

    ProgramHeader* p_headers = new ProgramHeader[header.phnum];
    bool valid_entry = false;
    for (uint32_t i = 0; i < header.phnum; i++) {
        file->read(header.phoff + header.phentsize * i, p_headers[i]);
        
        // check if this program header is a load operation. if not, skip
        if (p_headers[i].type != 1) {
            continue;
        }

        // check if this elf file tries to load a program outside the user range
        if (p_headers[i].vaddr < 0x80000000 || p_headers[i].vaddr + p_headers[i].memsz >= 0xF0001000 || p_headers[i].vaddr + p_headers[i].memsz - 1 < p_headers[i].vaddr - 1) {
            delete[] p_headers;
            return -1;
        }

        if (header.entry >= p_headers[i].vaddr && header.entry < p_headers[i].vaddr + p_headers[i].memsz) {
            valid_entry = true;
        }
    }

    // entry point cannot be in undefined memory
    if (!valid_entry) {
        delete[] p_headers;
        return -1;
    }

    delete[] p_headers;

    return 1;
}

uint32_t ELF::load(Node* file) {
    ElfHeader header;
    file->read(0, header);

    // valid_load should already be called, meaning this should be loading something that is VALID

    ProgramHeader* p_headers = new ProgramHeader[header.phnum];
    for (uint32_t i = 0; i < header.phnum; i++) {
        file->read(header.phoff + header.phentsize * i, p_headers[i]);
    }

    PCB* pcb = active_pcbs.mine();
    for (uint32_t i = 0; i < header.phnum; i++) {
        // check if this program header is a load operation. if not, skip
        if (p_headers[i].type != 1) {
            continue;
        }

        uint32_t aligned_vaddr = (p_headers[i].vaddr >> 12) << 12;
        uint32_t aligned_vaddr_end = (((p_headers[i].vaddr + p_headers[i].memsz - 1) >> 12) + 1) << 12;
        uint32_t aligned_memsz = aligned_vaddr_end - aligned_vaddr;
        pcb->queue->add_vme(new VME(aligned_vaddr, aligned_memsz));
        file->read_all(p_headers[i].offset, p_headers[i].filesz, (char*)p_headers[i].vaddr);
    }

    delete[] p_headers;

    return header.entry;
}