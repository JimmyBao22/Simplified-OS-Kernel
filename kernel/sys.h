#ifndef _SYS_H_
#define _SYS_H_

#include "pcb.h"

class SYS {
public:
    static void init(void);
    static void do_exit(int rc);
};

extern "C" void resume(UserContext* user_context);

extern void switch_processes(UserContext user_context);

extern void exit(uint32_t value);

extern int32_t sigreturn();

#endif
