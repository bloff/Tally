/*
 * Legacy module wrapper declaration retained for reference while the active
 * runtime uses gcctally_module_wrapper from minithread_api.h.
 */
#ifndef MODULES_H
#define MODULES_H

#include <stdint.h>


//this struct should be included at the top of every module struct, not by pointer but by reference
//this struct will be malloced and minithread will have a pointer to it 
struct gcctally_module_wraper{
    //pointer to the start of the memory location that this module can use
    void* start;
    //size of that chunk of memory
    uint64_t size;

    //function pointers provided in minihterad_init by MinithreadModuleArg
    //this functions should not be instrumented as they are run by the minithread lib side
    void (*init)();     
    void (*clean)();

    //init by thread init using the modules name provided in MinithreadModuleArg, it is used to sort and binary search the module when the struct is needed by its module api calls
    uint32_t hash;
};


#endif //MODULES_H
