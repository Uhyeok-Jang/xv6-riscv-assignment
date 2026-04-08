#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    // 다양한 nice 값으로 스케줄러 동작 관찰
    int nices[] = {0, 10, 20, 30, 39};
    int num_procs = 5;

    printf("EEVDF Scheduler 테스트 시작\n");

    for (int i = 0; i < num_procs; i++)
    {
        int pid = fork();

        if (pid < 0)
        {
            // 우선순위 부여
            printf("fork 실패 at i=%d\n", i);
            exit(1);
        }

        if (pid == 0)
        {
            // CPU 계속 요구
            while (1)
            {
                volatile int counter = 0;
                while (counter < 50000000)
                {
                    counter++;
                }
            }
            exit(0);
        }
        if (setnice(pid, nices[i]) < 0)
        {
            printf("setnice 실패: pid=%d nice=%d\n", pid, nices[i]);
        }
    }

    printf("백그라운드 프로세스 생성\n");
    printf("ps 실행\n");

    exit(0);
}