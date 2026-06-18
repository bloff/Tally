/*
 * Runtime helper API for estimating how much tally work a minithread consumed
 * during scheduler experiments.
 */
#ifndef _CYCLES_PROBE_
#define _CYLCES_PROBE_

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "minithread.h"

typedef struct stMinithreadInfo* MinithreadInfo;

struct stMinithreadInfo{
    //the number of cycles that the minithread is suposted to run on each iteration
    //this shoud be set in the begining nas left that way
    //this hsould be the base line of cycles to run before any optimization
    int cyclesIntended;

    //the true sum of work done by the minithread
    //unsigned as thwe work cannot be less that 0
    uint64_t trueCycleSum;

    //array that stores the true cycles ran by the minithread on each iteration
    int* trueCycleHistory;
};
MinithreadInfo minithread_info_init(Minithread, MinithreadInfo);

int findTrueWork(Minithread);
int addTrueWork(Minithread, MinithreadInfo);
void initCycleHistory(MinithreadInfo, int);

#endif //_CYCLES_PROBE_
