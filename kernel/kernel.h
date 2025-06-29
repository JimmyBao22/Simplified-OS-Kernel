#ifndef _KERNEL_H_
#define _KERNEL_H_

#include "future.h"
#include "ext2.h"
#include "pcb.h"

Future<int> kernelMain(void);

extern Ext2* fs;

// stores all active pcbs
extern PerCPU<PCB*> active_pcbs;

extern PerCPU<bool> interrupts;

#endif
