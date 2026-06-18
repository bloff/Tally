/*
 * Internal data structures backing minithread state, loaded code, and modules.
 */
#ifndef _MINITHREAD_STRUCT
#define _MINITHREAD_STRUCT

#include <stdint.h>

typedef struct minithreadFunc* MinithreadCode;
typedef void* MinithreadModules;

typedef enum {
    MINITHREAD_NEW = 0,
    MINITHREAD_INTERRUPTED = 1,
    MINITHREAD_RUNNING = 2,
    MINITHREAD_FORCE_YIELD = 3,
    MINITHREAD_VOLUNTARY_YIELD = 4,
    MINITHREAD_ERRORED = 5,
    MINITHREAD_RETURNED = 6
} MinithreadState;

//aka Minithread
struct stMinithread {
    /// stack-pointer for the minithread
    void **sp;
    
    ///usefull for cheking how much memory is in use (bottom of the stack) 
    void **sp_base;   
    
    /// position of memory allocated for the stack (which happens to be the top of the stack)
    void **stack;

    /// last position of memory usesd by modules, this means that sp can go from stack to stack top - 1
    void **stack_top;

    //paramm given to the initial function
    void* init_Args;

    /// ammount of memory allocated for the stack (in terms of sizeof(void*)-long words)    
    uint64_t memory_size_bytes;
    
    /// whether the thread is currently stopped, running, paused or interrupted
    MinithreadState state;
    
    /// the function to be run when the thread starts
    MinithreadCode body;

    //amount of cycles needed to run, this number does not change during execution of the thread
    int64_t cycles_to_run;
    //amount of cycles left when execution is stopped
    int64_t cycles_left;

    //array of modules all malloced, shoud be sorted by name hash
    MinithreadModules *modules;
    int n_modules;

};

//struct that stores the dinamicly opend instruemnted file and its function entry point
struct minithreadFunc {
    //reference for dlopen
    void *handler;
    //entry function signature
    void (*fptr)(void*);
    
    char* file_name;
    char* func_name;
    //thread id, used for magic_dl and linking diferent versions of the same obj file
    uint16_t id;
    uint64_t refcount;
    MinithreadCode cache_next;
};

#endif
