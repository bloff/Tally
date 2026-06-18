/*
 * Experimental alternate context-switch implementation that avoids relying on
 * thread-local access after switching stacks.
 */
#include "minithread.h"
#include "minithread_api.h"

#include <stdlib.h>
#include <memory.h>
#include <stdio.h>
#include <assert.h>

_Thread_local Minithread threadInUse; 

/*
*   @brief Runs the minithread for the amount of cycles in the minithread params, this function will do the context switch and back
*   This is an alternetive implementation of the context switch were we don't relie on a thread_local variable being accessible after a change of stack pointers
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
    }else if(thread->state == MINITHREAD_PAUSED || thread->state == MINITHREAD_NEW){ 
        mt_arg = thread->cycles_to_run;
    }else if(thread->state == MINITHREAD_STOPPED){
        //the context switch happens not because of the instrmented mecanism but because of a macro like error or yeild
        mt_arg = thread->cycles_left;
    }else{
        fprintf(stderr, "unknown minithread state %d\n", thread->state);
        return;  
    }
    
    //if the amount of cycles to run is less then 0 then the thread is not suposed to run
    if(mt_arg <= 0){
        threadInUse = NULL;
        return;            
    }

    register void *_thread_ptr asm("r14");

    //variables to use in extended asm
    _thread_sp = thread->sp;
    _thread_ptr = thread;

    if(thread->state == MINITHREAD_NEW) {
        thread->state = MINITHREAD_RUNNING;
        
        PUSH_REGISTERS;
        // Saves my stack-pointer into a thread-local variable
        asm volatile("\tmovq %%rsp, %0\n" : "=m" (_my_sp) );
        // Copies the thread's stack-pointer into the rsp register
        // This changes the stack to the stack of the thread
        asm volatile("\tmovq %0, %%rsp\n" : : "m" (_thread_sp) );

        asm volatile("pushq $0");
        ((Minithread)_thread_ptr)->body->fptr( ((Minithread)_thread_ptr)->init_Args);

    }else{
        thread->state = MINITHREAD_RUNNING;

        //we are saving the registers of this context
        PUSH_REGISTERS;

        // Saves my stack-pointer into a thread-local variable
        asm volatile("\tmovq %%rsp, %0\n" : "=m" (_my_sp) );
        // Copies the thread's stack-pointer into the rsp register
        // This changes the stack to the stack of the thread
        asm volatile("\tmovq %0, %%rsp\n" : : "m" (_thread_sp) );

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

    //31 is the size of the registers saveds + the initial buffer of 0 in the stack
    if(thread->sp + 31 == thread->sp_base)
        thread->state = MINITHREAD_RETURNED;
    
    uint64_t cycles_left = 0;
    //deppeding on how the thread exited we clean its intruction pointer and state
    switch(thread->state){
        case MINITHREAD_RUNNING:
        case MINITHREAD_YIELDED:
            cycles_left = mt_arg;
            thread->cycles_left = cycles_left;            
            
            #ifdef gcctally_DEBUG_RUNTIME 
                printf("state: thread running\ncycles left: %d\n", cycles_left); 
            #endif

            thread->state = MINITHREAD_PAUSED;
            //thread->ip = _break_sp;
            break;

        //when the thread state is stopped we do not reset its cycles
        //as the contextswitch was caused by a macro that wants to preserve the cycles the he did not run 
        case MINITHREAD_STOPPED:
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
