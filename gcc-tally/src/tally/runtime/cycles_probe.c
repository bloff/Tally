/*
 * Library runtime helper for measuring requested versus observed tally cycles.
 */
#include "cycles_probe.h"

#include "minithread.h"
#include "minithread_api.h"

MinithreadInfo minithread_info_init(Minithread thread, MinithreadInfo t_info){
    MinithreadInfo ret;
    if(t_info == NULL){
        ret = (MinithreadInfo) malloc(sizeof (struct stMinithreadInfo));
    }else{
        ret = t_info;
    }

    ret->trueCycleSum = 0;
    ret->cyclesIntended = thread->cycles_to_run;

    return ret;
}

int findTrueWork(Minithread thread){
    int cycles_left = thread->cycles_left;
    int cycles_wanted = thread->cycles_to_run;

    return cycles_wanted - cycles_left; 
}

int addTrueWork(Minithread thread, MinithreadInfo t_info){
    int work = findTrueWork(thread);
    t_info->trueCycleSum += work;
    return work;
}

void initCycleHistory(MinithreadInfo t_info, int history_size){
    t_info->trueCycleHistory = malloc(sizeof(int) * history_size);
}
