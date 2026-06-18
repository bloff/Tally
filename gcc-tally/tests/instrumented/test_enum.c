/*
 * Instrumented compiler probe that checks enum handling after Tally's GCC
 * plugin rewrites control-flow blocks.
 */
#include <stdio.h>

typedef enum{
    a = 0,
    b = 1,
    c = 2
} tt;

void f(tt p){
    switch(p){
        case a:
            printf("a");
            break;
        case b:
            printf("b");
            break;
        case c:
            printf("c");
            break;
        default:
            printf("default");
    }
}

int main(){
    f(a);
    f(b);
    f(c);
}
