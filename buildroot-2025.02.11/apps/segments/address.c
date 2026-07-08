#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

int init_data = 30;
int noinit_data;

void prints(void){
    int local_data = 1;
    printf("\nPid of the process is = %d", getpid());
    printf("\nAddresses which fall into:");
    printf("\n 1) Code  segment = %p", prints);
    printf("\n 2) Data  segment = %p", &init_data);
    printf("\n 3) BSS   segment = %p", &noinit_data);
    printf("\n 4) Stack segment = %p\n", &local_data);
}

int main(){
    prints();

    return 0;
}
