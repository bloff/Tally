/*
 * Instrumented compiler probe for the HASH/HASH_S helpers used by the module
 * lookup API.
 */

#include<stdio.h>

#include "minithread_api.h"

const int a = HASH_S("HELLO");

int main(){
    //printf("%d %d\n", HASH_S("hello"), HASH("yoo"));
    printf("%d\n", a);
}
