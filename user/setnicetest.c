#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  int me = getpid();
  int before = getnice(me);
  printf("자신(변경 전): pid=%d nice=%d\n", me, before);

  int r = setnice(me, 10);
  printf("setnice(self,10) ret=%d, nice=%d\n", r, getnice(me));

  // out of range
  r = setnice(me, -1);
  printf("setnice(self,-1) ret=%d (expect: -1), nice=%d\n", r, getnice(me));
  r = setnice(me, 40);
  printf("setnice(self,40) ret=%d (expect: -1), nice=%d\n", r, getnice(me));

  // pid 존재 x
  r = setnice(99999, 5);
  printf("setnice(99999,5) ret=%d (expect: -1)\n", r);

  
  int child = fork();
  if(child < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(child == 0){
    int cpid = getpid();
    printf("자식(초기): pid=%d nice=%d\n", cpid, getnice(cpid));
    // let parent set our nice
    pause(50);
    printf("자식(부모 설정 후): pid=%d nice=%d\n", cpid, getnice(cpid));
    exit(0);
  }

  // 여기서 세팅
  pause(1);
  r = setnice(child, 7);
  printf("부모: setnice(child=%d,7) ret=%d\n", child, r);
  printf("부모가 보는 자식: pid=%d nice=%d\n", child, getnice(child));
  wait(0);

  exit(0);
}
