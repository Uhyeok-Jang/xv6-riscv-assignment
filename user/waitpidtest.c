#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(void)
{
    int child = fork();
    if (child < 0)
    {
        printf("fork failed\n");
        exit(1);
    }

    if (child == 0)
    {
        pause(50);
        exit(0);
    }

    printf("waitpid(child=%d) -> %d\n", child, waitpid(child));
    printf("waitpid(invalid;99999) -> %d\n", waitpid(99999));
    printf("waitpid(reaped child=%d) -> %d\n", child, waitpid(child));
    exit(0);
}