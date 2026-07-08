#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

int init_data = 30;
int noinit_data;

extern char __executable_start;
extern char __etext;
extern char _edata;
extern char __bss_start;
extern char _end;

void prints(void){
    int local_data = 1;
    printf("\nPid of the process is = %d", getpid());
    printf("\nAddresses which fall into:");
    printf("\n 1) __executable_start   = %p", &__executable_start);
    printf("\n 2) This function        = %p",               prints);
    printf("\n 3) __etext              = %p",            &__etext);
    printf("\n 4) init_data variable   = %p",          &init_data);
    printf("\n 5) _edata               = %p",             &_edata);
    printf("\n 6) __bss_start          = %p",        &__bss_start);
    printf("\n 7) noinit_data variable = %p",        &noinit_data);
    printf("\n 8) _end                 = %p",               &_end);
    printf("\n 9) local_data variable  = %p\n",       &local_data);
}

int main(){
    prints();

    return 0;
}
