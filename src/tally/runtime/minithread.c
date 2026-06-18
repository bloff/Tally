/*
 * Core Tally minithread runtime: loads instrumented functions, owns private
 * stacks/modules, and performs AMD64 context switches.
 */
#include "minithread.h"
#include "minithread_api.h"

#include <stdlib.h>
#include <memory.h>
#include <stdio.h>
#include <assert.h>

#ifndef TALLY_SOURCE_DIR
#define TALLY_SOURCE_DIR "."
#endif


void _minithread_reset_stack(Minithread thread);
static int append(char **str, const char *buf, int size);
int _minithread_comp(const void* a, const void* b);

//this variable will be set when calling minithread_run_cycle, when exiting the function the value will return to NULL
//this is usefull when runnning instrumented code and we need a self reference to the minithread
_Thread_local Minithread threadInUse; 

//variable not used in this version of the library, here as the fundation to expand to multithreading
_Thread_local uint16_t id;

/*
*   @brief Uses dlopen to load the function needed, allocates and sets up the MinithreadCode wrapper struct, it can also compile and instrument the file were the function is located, this is not advised as system calls are costly
*   @param f Wrapper struct that contains all the information about the function
*   @return Returns the MinihtreadCode wrapper function with everything configured
*/
MinithreadCode _load_func(MinithreadFuncArg f){
    MinithreadCode code;
    code = (MinithreadCode) malloc(sizeof (struct minithreadFunc));

    printf("loading file %s %s\n", f->file_name, f->func_name);

    asprintf(&(code->file_name), "%s", f->file_name);
    asprintf(&(code->func_name), "%s", f->func_name);

    if(!f->compiled){
        code->id = id;
        
        //create the command and run the script
        char *command;
        asprintf(&command, "bash \"%s/scripts/build-instrumented.sh\" %s", TALLY_SOURCE_DIR, f->file_name);
        
        #ifdef gcctally_DEBUG_RUNTIME
            printf("calling: %s\n", command);
        #endif

        int status = system(command);
        if(status != 0){
            fprintf(stderr, "ERROR: failed to compile %s with status %d\n", f->file_name, status);
            free(command);
            abort();
        }

        //clean the memory
        free(command);
    }

    //load the file compiled in the script
    char *path;
    asprintf(&path, "%s/dl/%s.so", TALLY_SOURCE_DIR, f->file_name);
    code->handler = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if(code->handler == NULL){
        fprintf(stderr, "ERROR: unable to load %s: %s\n", path, dlerror());
        free(path);
        abort();
    }

    free(path);

    #ifdef gcctally_DEBUG_RUNTIME
        printf("dlopen error = %s\n", dlerror());
    #endif

    //find the symbol to the entry point of the function
    code->fptr = (void (*)(void*)) dlsym(code->handler, f->func_name);
    if(code->fptr == NULL){
        printf("ERROR: unable to find the function %s\n", f->func_name);
        abort();
    } 

    return code;
}

/*
*   @brief Clean the minithread memory
*   @param thread Minithread to join
*/
void minithread_join(Minithread thread){
    //free the memory of the minithread
    free(thread->stack);
    
    //free the func struct 
    //at this time we need to delete this memory as each minithread as a struct to it self
    //TODO: write a func manager so that there is only one func struct per func obj/thread
    dlclose(thread->body->handler);
    free(thread->body->file_name);
    free(thread->body->func_name);
    free(thread->body);
}

/*
 *  @brief Allocates memory in the stack of minithread for the module, sets up all needed variables and calls the module init function.
 *  @param thread Minihtread were we are going to load the module
 *  @param module Same wrapper provided to minithread_init that contains all the information needed to load the module
 *  @param i Index for the module struct in the array of modules in the minithread
 */
void _minithread_load_module(Minithread thread, MinithreadModuleArg module, int i){
    #ifdef gcctally_DEGUB_RUNTIME
        printf("loading module\n");
    #endif

    typedef struct gcctally_module_wrapper* gccModule;
    thread->modules[i] = malloc(module->struct_size);

    //poor man's inheritance
    gccModule wrapper = (gccModule) thread->modules[i]; 

    //has its not a static string we cannot use HASH_S
    wrapper->hash = HASH(module->module_name);

    #ifdef gcctally_DEBUG_RUNTIME
        printf("hashed %d\n", wrapper->hash);
    #endif

    wrapper->init = module->init;
    wrapper->clean = module->clean;

    wrapper->size = -1;
    wrapper->start = thread->stack_top;
    
    #ifdef gcctally_DEBUG_RUNTIME
        printf("stack %d %d %d\n", thread->sp, thread->stack_top, thread->sp - thread->stack_top);
    #endif

    //some modules don't need a inicialization function
    if(wrapper->init != NULL){
        wrapper->init((void*) thread->modules[i], module->init_args);
        //ensure that init as updated the size variable
        assert(("init has not updated module size", wrapper->size != -1));
        thread->stack_top += wrapper->size;
    }

}
/* 
 *  @brief Creates and loads everything needed to launch a minithread  
 *  @param _thread If _thread is null then the function will allocate memory for a new minithread, if not the function will use de pointer to the struct supplied
 *  @param mem_size Size of the private stack allocated to the minithread
 *  @param init_args If the target function of the minithread needs input arguments supplie the struct pointer here, minithread_run_cycle will run f(init_args)
 *  @param f Wrapper struct that contains all the information needed to load the target function to the minithread
 *  @param modules Array of wrapper structs that contain all the information needed to load modules to the minithread
 *  @param n_modules Size of the modules array
 *  @param cycles Number of cycles that the minithread is going to run when calling minithread_run_cycle
 *  @return If _thread is null it will return the pointer to the new allocated memory, else it will return _thread
 * 
*/
Minithread minithread_init(Minithread _thread, uint64_t mem_size, void* init_args,  MinithreadFuncArg f, MinithreadModuleArg *modules, int n_modules, int64_t cycles){
   
    Minithread thread;
    if(_thread == NULL){
        thread = (Minithread) malloc(sizeof (struct stMinithread));
    }else{
        thread = _thread;
    }

    //allocate the total memory used by the minithread
    thread->memory_size_bytes = sizeof(void*) * mem_size;
    thread->stack = malloc(thread->memory_size_bytes);
    
    //stack top is at the same position as stack as there is still no module loaded up in the minithread
    thread->stack_top = thread->stack;
    _minithread_reset_stack(thread);


    #ifdef gcctally_DEBUG_RUNTIME
        printf("stack reseted\n");
    #endif

    //setup stack
    thread->sp_base = thread->sp;

    thread->cycles_to_run = cycles;
    thread->cycles_left = 0;

    thread->init_Args = init_args;

    //loading modules
    //create the array that will hold all module pointers
    //this string will contain the path to every file that the modules need
    char* modules_path = NULL; 

    if(modules != NULL){
        thread->n_modules = n_modules;
        thread->modules = malloc( n_modules * sizeof(void*));
	    
        #ifdef gcctally_DEBUG_RUNTIME
            printf("modules malloced %d\n", n_modules);
        #endif

        int n_module_files;
        for(int i = 0; i < n_modules; i++){
            _minithread_load_module(thread, modules[i], i);
        }

        #ifdef gcctally_DEBUG_RUNTIME
            printf("sorting\n");
        #endif
        //after initing all the modules we need to sort then so that we can use the find function
        qsort(thread->modules, n_modules, sizeof(void*), _minithread_comp);
    }else{
        thread->modules = NULL;
        thread->n_modules = 0;
    }
    
    thread->body = _load_func(f);
    thread->state = MINITHREAD_NEW;  

    return thread;
}

/*
*   @brief Runs the minithread for the amount of cycles in the minithread params, this function will do the context switch and back
*   @param _thread minithread to be run
*/
void minithread_run_cycle(Minithread _thread){
    static _Thread_local Minithread thread = 0;
    static _Thread_local Ptr _my_sp = 0;
    static _Thread_local Ptr _thread_sp = 0;
    static _Thread_local Ptr _thread_ip = 0;
    static _Thread_local Ptr _break_value = 0;
    static _Thread_local Ptr _break_sp = 0;

    thread = _thread;
    threadInUse = _thread;

    //load the special counter with cycles
    //defined in a ifdef in .h
    //if thread is pauses then the counter should continue as it was when paused
    if(thread->state == MINITHREAD_ERRORED || thread->state == MINITHREAD_RETURNED){
        fprintf(stderr, "trying to run thread that cannot run anymore\n");
        return; 
    }else if(thread->state == MINITHREAD_VOLUNTARY_YIELD || thread->state == MINITHREAD_NEW){
        mt_arg = thread->cycles_to_run;
    }else if(thread->state == MINITHREAD_FORCE_YIELD){
        // Forced yields can overshoot the exact budget because tally checks happen
        // at instrumented basic-block boundaries. Carry that negative balance
        // forward so the next cycle starts with the debt already subtracted.
        mt_arg = thread->cycles_to_run + thread->cycles_left;
    }else if(thread->state == MINITHREAD_INTERRUPTED){
        //the context switch happens not because of the instrmented mecanism but because of a macro like error or yeild
        mt_arg = thread->cycles_left;
    }else{
        fprintf(stderr, "unknown minithread state %d\n", thread->state);
        return;  
    }
    
    //if the amount of cycles to run is less then 0 then the thread is not suposed to run
    if(mt_arg <= 0){
        if(thread->state == MINITHREAD_FORCE_YIELD){
            thread->cycles_left = mt_arg;
        }
        threadInUse = NULL;
        return;            
    }

    //variables to use in extended asm
    _thread_sp = thread->sp;

    //we are saving the registers of this context
    PUSH_REGISTERS;

    // Saves my stack-pointer into a thread-local variable
    asm volatile("\tmovq %%rsp, %0\n" : "=m" (_my_sp) );
    // Copies the thread's stack-pointer into the rsp register
    // This changes the stack to the stack of the thread
    asm volatile("\tmovq %0, %%rsp\n" : : "m" (_thread_sp) );

    //lauching the thread
    //giving the microthread the context
    //ATTENTION: with the implementation of _thread_local variable in gcc is safe to use then after switching stacks
    //We don't know if this is a standard so in minithread_alternetive there is a implementation of this function where we don't relie on this assumption
    if (thread->state == MINITHREAD_NEW) {
        // If the thread needs to be started, do it by calling the function that defines the body of the thread.
        // This call is made on the minithread's own stack at this point.
        thread->state = MINITHREAD_RUNNING;
        (thread->body)->fptr(thread->init_Args);
    }
    else {
        thread->state = MINITHREAD_RUNNING;
        // If the thread is in an interrupted state, restore the (non-arg) registers and jump to the correct position.
        // restores the various registers from what's saved in the thread's stack
        POP_REGISTERS;
        
        // Continues thread execution, the return label is on top of the stack
        asm volatile("\tret\n");
    }

    //Target for the instrumentation to call a goTo and make the context switch back
    //By calling convention when a thread starts its context switch back it leaves at the top of the stack the label from where it left
    asm (
        "_minithread_break:\n"
    );
    
    //save the registers of our minithread function
    PUSH_REGISTERS;

    // Saves the thread's stack-pointer into a variable
    asm volatile("\tmovq %%rsp, %0\n" : "=m" (_thread_sp) : );

    // Restores my stack pointer
    asm volatile("\tmovq %0, %%rsp\n" : : "m" (_my_sp) );

    // Restores our(context switcher) registers
    POP_REGISTERS;

    //save the stack pointer
    thread->sp = _thread_sp;

    #ifdef gcctally_DEBUG_RUNTIME
        printf("stack pointer state sp: %d sp_base: %d stack_size: %d\n", thread->sp, thread->sp_base, thread->sp_base - thread->sp);
    #endif

    //30 is the size of the registers saveds
    if(thread->sp + 30 == thread->sp_base)
        thread->state = MINITHREAD_RETURNED;
    
    int64_t cycles_left = 0;
    //deppeding on how the thread exited we clean its intruction pointer and state
    switch(thread->state){
        case MINITHREAD_RUNNING:
            thread->state = MINITHREAD_FORCE_YIELD;
        case MINITHREAD_VOLUNTARY_YIELD:
            cycles_left = mt_arg;
            thread->cycles_left = cycles_left;            
            
            #ifdef gcctally_DEBUG_RUNTIME 
                printf("state: thread running\ncycles left: %d\n", cycles_left); 
            #endif

            //thread->ip = _break_sp;
            break;

        //when the thread state is stopped we do not reset its cycles
        //as the contextswitch was caused by a macro that wants to preserve the cycles the he did not run 
        case MINITHREAD_INTERRUPTED:
            //thread->ip = _break_sp;
            cycles_left = mt_arg;
            
            thread->cycles_left = cycles_left;   
            
            #ifdef gcctally_DEBUG_RUNTIME
                printf("thread force stopped");         
            #endif
           
            break;
 
        case MINITHREAD_RETURNED:
            break;
     
        case MINITHREAD_ERRORED:
            //TODO
            fprintf(stderr, "not implemented yet");
            break;
    };

    threadInUse = NULL;
}

/*
*   @brief Changes the amount of cycles that the minithread shoud run
*   @param thread Minihtread target
*   @param cycles New amount of cycles to run
*/
void minithread_change_cycles(Minithread thread, int64_t cycles){
    thread->cycles_to_run = cycles;
}

/*
*   @brief Zeros the minithread stack and sets the stack pointer to the last position on the stack array
*   @param thread Target minithread to reset stack
*/
void _minithread_reset_stack(Minithread thread) {
    memset((char*)thread->stack, 0, thread->memory_size_bytes);
    thread->sp = (void**)(thread->memory_size_bytes + (char*)thread->stack);
    //up one position to the stack base
    *--thread->sp = (void *)abort; /* needed for alignment only */
}

typedef struct gcctally_module_wrapper* gccModule;
/*
*   @brief Performs a binary search on the minihtread modules array to find the especified module
*   @param thread Target minithread were to search for the module
*   @param name_hash Name of the module to search
*   @return Returns the pointer to the module if it exists otherwise it return NULL
*/
void* minithread_m_find(Minithread thread, uint32_t name_hash){
    int m, l = 0, r = thread->n_modules - 1;


    while(l <= r){
        m = l + (r - l) / 2; 
        gccModule wrapper = (gccModule) thread->modules[m]; 

        if(wrapper->hash == name_hash)
            return thread->modules[m]; 

        if(wrapper->hash < name_hash) 
            l = m + 1;
        else
            r = m - 1;
    }

    return NULL;
}

/*
*   @brief Performs a binary search on the minihtreadInUse modules array to find the especified module
*   @param name_hash Name of the module to search
*   @return Returns the pointer to the module if it exists otherwise it return NULL
*/
void* minithread_find(uint32_t name_hash){
    Minithread thread = threadInUse;

    return minithread_m_find(thread, name_hash);

}

/*
*   @brief Helper comparison function to sort minithread modules on its array, is uses the modules name hash as the values to compare
*/
int _minithread_comp(const void* a, const void* b){
    typedef struct gcctally_module_wrapper* gccModule;
    gccModule module_a = *(gccModule const *)a;
    gccModule module_b = *(gccModule const *)b;

    if(module_a->hash < module_b->hash) return -1;
    if(module_a->hash > module_b->hash) return 1;
    return 0;
}
