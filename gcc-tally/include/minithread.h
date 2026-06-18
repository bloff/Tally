/*
 * Public minithread runtime API and AMD64 context-save macros.
 */
#ifndef _MINITHREAD_H
#define _MINITHREAD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <dlfcn.h>

#include "flags.h"
#include "minithread_struct.h"

#ifndef __amd64
#error "Only works with AMD64, currently" //can i suppress this? working on mac m1, it drives me nuts
#endif

asm("\t.globl _minithread_break\n");
asm("\t.type _minithread_break @function\n");

typedef struct stMinithread* Minithread;

typedef struct minithreadFuncOpt* MinithreadFuncArg;
typedef struct minithreadModuleOpt* MinithreadModuleArg;

typedef void *Ptr;

#if __cplusplus
extern "C" {
#endif

register int64_t mt_arg asm("r15");

/*
 * this will be updated every time a new thread is context switched in
 * its a target for macros as RAISE_ERROR or PAUSE
*/
extern _Thread_local Minithread threadInUse; 

//id used for _load_func, updated for every thread
extern _Thread_local uint16_t id;

/*
    still need to figure it out the macros
    #define _MINITHREAD_RAISE_ERROR used in allocation 
    #define _MINITHREAD_STOP -> switch back but does not reset the special_counter
    #define _MINITHREAD_YEILD -> switch back special_counter is reseted
*/

//save the register from a context switch
#define PUSH_REGISTERS asm (\
    "\tpushq %r14\n"\
    "\tpushq %r13\n"\
    "\tpushq %r12\n"\
    "\tpushq %r11\n"\
    "\tpushq %r10\n"\
    "\tpushq %r9\n"\
    "\tpushq %r8\n"\
    "\tpushq %rbp\n"\
    "\tpushq %rdi\n"\
    "\tpushq %rsi\n"\
    "\tpushq %rbx\n"\
    "\tpushq %rdx\n"\
    "\tpushq %rcx\n"\
    "\tpushq %rax\n"\
    "\tsub $128, %rsp\n" /* 16 (8 * 2) (128 bit per float register) * 8 */  \
    "\tmovhps %xmm0, -128(%rsp)\n"\
    "\tmovlps %xmm0, -120(%rsp)\n"\
    "\tmovhps %xmm1, -112(%rsp)\n"\
    "\tmovlps %xmm1, -104(%rsp)\n"\
    "\tmovhps %xmm2, -96(%rsp)\n"\
    "\tmovlps %xmm2, -88(%rsp)\n"\
    "\tmovhps %xmm3, -80(%rsp)\n"\
    "\tmovlps %xmm3, -72(%rsp)\n"\
    "\tmovhps %xmm4, -64(%rsp)\n"\
    "\tmovlps %xmm4, -56(%rsp)\n"\
    "\tmovhps %xmm5, -48(%rsp)\n"\
    "\tmovlps %xmm5, -40(%rsp)\n"\
    "\tmovhps %xmm6, -32(%rsp)\n"\
    "\tmovlps %xmm6, -24(%rsp)\n"\
    "\tmovhps %xmm7, -16(%rsp)\n"\
    "\tmovlps %xmm7, -8(%rsp)\n"\
    );

//put the registers saved in the last context switch to the stack again on the registers
//each xmm register as 128 bits (16 bytes) so we need to load first the top half then the lower half
#define POP_REGISTERS asm (\
    "\tadd $128, %rsp\n" /* 16 (8 * 2) (128 bit per float register) * 8 */  \
    "\tmovhps -128(%rsp), %xmm0\n"\
    "\tmovlps -120(%rsp), %xmm0\n"\
    "\tmovhps -112(%rsp), %xmm1\n"\
    "\tmovlps -104(%rsp), %xmm1\n"\
    "\tmovhps -96(%rsp), %xmm2\n"\
    "\tmovlps -88(%rsp), %xmm2\n"\
    "\tmovhps -80(%rsp), %xmm3\n"\
    "\tmovlps -72(%rsp), %xmm3\n"\
    "\tmovhps -64(%rsp), %xmm4\n"\
    "\tmovlps -56(%rsp), %xmm4\n"\
    "\tmovhps -48(%rsp), %xmm5\n"\
    "\tmovlps -40(%rsp), %xmm5\n"\
    "\tmovhps -32(%rsp), %xmm6\n"\
    "\tmovlps -24(%rsp), %xmm6\n"\
    "\tmovhps -16(%rsp), %xmm7\n"\
    "\tmovlps -8(%rsp), %xmm7\n"\
    "\tpopq %rax\n"\
    "\tpopq %rcx\n"\
    "\tpopq %rdx\n"\
    "\tpopq %rbx\n"\
    "\tpopq %rsi\n"\
    "\tpopq %rdi\n"\
    "\tpopq %rbp\n"\
    "\tpopq %r8\n"\
    "\tpopq %r9\n"\
    "\tpopq %r10\n"\
    "\tpopq %r11\n"\
    "\tpopq %r12\n"\
    "\tpopq %r13\n"\
    "\tpopq %r14\n"\
    );

struct minithreadFuncOpt{
    char *file_name, *func_name;
    //set to 1 if it is not needed compile again
    int compiled;
};

struct minithreadModuleOpt{
    char* module_name; 
    
    //           struct ptr, func args
    void (*init)(void*, void*);
    void (*clean)();    

    void* init_args;

    uint64_t unique_id;

    uint64_t struct_size;
};

Minithread minithread_init(Minithread _thread, uint64_t mem_size, void* init_args, MinithreadFuncArg f, MinithreadModuleArg *modules, int n_modules, int64_t cycles);
void minithread_join(Minithread thread);
void minithread_run_cycle(Minithread _thread);
void minithread_change_cycles(Minithread _thread, int64_t cycles);

#if __cplusplus
}
#endif

#endif //MINITHREAD_H
