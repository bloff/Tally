/*
 * API and macros exposed to instrumented code so it can find modules and
 * context-switch back to the minithread manager.
 */
#ifndef _MINITHREAD_API
#define _MINITHREAD_API

//file provided to module non instrumented files with helper functions for api module calls

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "minithread_struct.h"

typedef struct stMinithread* Minithread;
extern _Thread_local Minithread threadInUse; 

//see: https://stackoverflow.com/a/25646689
#define H1(s,i,x)   (x*65599u+(uint8_t)s[(i)<strlen(s)?strlen(s)-1-(i):strlen(s)])
#define H4(s,i,x)   H1(s,i,H1(s,i+1,H1(s,i+2,H1(s,i+3,x))))
#define H16(s,i,x)  H4(s,i,H4(s,i+4,H4(s,i+8,H4(s,i+12,x))))
#define H64(s,i,x)  H16(s,i,H16(s,i+16,H16(s,i+32,H16(s,i+48,x))))
#define H256(s,i,x) H64(s,i,H64(s,i+64,H64(s,i+128,H64(s,i+192,x))))

#define H1_S(s,i,x)   (x*65599u+(uint8_t)s[(i)<sizeof(s)?sizeof(s)-1-(i):sizeof(s)])
#define H4_S(s,i,x)   H1(s,i,H1(s,i+1,H1(s,i+2,H1(s,i+3,x))))
#define H16_S(s,i,x)  H4(s,i,H4(s,i+4,H4(s,i+8,H4(s,i+12,x))))
#define H64_S(s,i,x)  H16(s,i,H16(s,i+16,H16(s,i+32,H16(s,i+48,x))))
#define H256_S(s,i,x) H64(s,i,H64(s,i+64,H64(s,i+128,H64(s,i+192,x))))

//hash function for variable strings
#define HASH(s)    ((uint32_t)(H256(s,0,0)^(H256(s,0,0)>>16)))
//hash function for static strings, can be claculated compile-time
#define HASH_S(s)    ((uint32_t)(H256(s,0,0)^(H256(s,0,0)>>16)))

#define MINITHREAD_STRINGIFY(x) #x      //stringify argument
#define MINITHREAD_STR(x) MINITHREAD_STRINGIFY(x)  //indirection to expand argument macros

#define _MINITHREAD_INTERRUPT(label)\
    threadInUse->state=1;\
    asm volatile("add $1, %r15");\
    asm volatile("push ._minithread_stop_label_" MINITHREAD_STR(label) "@GOTPCREL(%rip)");\
    asm volatile("push _minithread_break@GOTPCREL(%rip)");\
    asm volatile("ret");\
    asm volatile("._minithread_stop_label_" MINITHREAD_STR(label) ":");

#define _MINITHREAD_YEILD(label)\
    threadInUse->state=4;\
    asm volatile("add $1, %r15");\
    asm volatile("push ._minithread_yeild_label_" MINITHREAD_STR(label) "@GOTPCREL(%rip)");\
    asm volatile("push _minithread_break@GOTPCREL(%rip)");\
    asm volatile("ret");\
    asm volatile("._minithread_yeild_label_" MINITHREAD_STR(label) ":");

#define _MINITHREAD_ERROR(label)\
    threadInUse->state=5;\
    asm volatile("push _minithread_break@GOTPCREL(%rip)");\
    asm volatile("ret");

#define MINITHREAD_STOP _MINITHREAD_STOP(__COUNTER__)
#define MINITHREAD_YEILD _MINITHREAD_YEILD(__COUNTER__)
#define MINITHREAD_ERROR _MINITHREAD_ERROR(__COUNTER__)


typedef struct stMinithread* Minithread;

//TODO idk if it can bind on dlopen to a minithread funciton
//find module by its name
void* minithread_find(uint32_t name_hash);
void* minithread_m_find(Minithread thread, uint32_t name_hash);

//this struct should be included at the top of every module struct, not by pointer but by reference
//this struct will be malloced and minithread will have a pointer to it 
struct gcctally_module_wrapper{
    //start and size shoud be updated when lauching the function init
    //if size stays at -1 then an error will be raised

    //pointer to the start of the memory location that this module can use
    void* start;
    //size of that chunk of memory
    uint64_t size;

    //function pointers provided in minihterad_init by MinithreadModuleArg
    //this functions should not be instrumented as they are run by the minithread lib side
    void (*init)(void* module_struct, void* args);     
    void (*clean)();

    //init by thread init using the modules name provided in MinithreadModuleArg, it is used to sort and binary search the module when the struct is needed by its module api calls
    uint32_t hash;
};

#endif //_MINITHREAD_API
