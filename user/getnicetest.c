#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(void)
{
    int me = getpid();
    int v = getnice(me);

    // 실행 중인 거 실험
    printf("자신: pid=%d nice=%d\n", me, v);

    // 유효하지 않은 거 실험
    int invalid = 99999;
    printf("Invalid: pid=%d nice=%d\n", invalid, getnice(invalid));

    // 자식 생성
    int child = fork();
    if (child < 0)
    {
        printf("fork failed\n");
        exit(1);
    }

    if (child == 0)
    {   
        // 자식 거 확인
        int cpid = getpid();
        printf("자식: pid=%d nice=%d\n", cpid, getnice(cpid));
        pause(50);
        exit(0);
    }

    // 부모 입장에서 정상 조회 가능 실험
    pause(1);
    printf("부모가 보는 자식: pid=%d nice=%d\n", child, getnice(child));

    wait(0);

    // wait 이후 proc 엔트리가 정리되고 getnice -1 반환 확인
    printf("wait이후: getnice(child)=%d\n", getnice(child));

    exit(0);
}
