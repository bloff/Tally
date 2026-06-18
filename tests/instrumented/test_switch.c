/*
 * Instrumented compiler probe that keeps switch statements covered by the
 * Tally GCC plugin's cost model and inserted break paths.
 */
#include <stdio.h>
#include <stdint.h>

extern int64_t counter;

int a(){
    return 1337;
}

int b(){
    return 42;
}

int c(){
    return 69;
}

void t(void* args){
    int x = (int)(intptr_t) args;
    int sum = 0;

    switch (x)
    {
    case 1:
        sum = a();        
        /* fall through */

    case 2:
        sum = b();
        /* fall through */

    case 3:
        sum = c();
    }

}
