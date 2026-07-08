#include <stdio.h>
#include <sys/resource.h>

int main() {
    struct rlimit rlim;

    if (getrlimit(RLIMIT_STACK, &rlim))
        return 1;
    printf("stack size: current %ld max: %ld\n", rlim.rlim_cur, rlim.rlim_max);
    return 0;
}
