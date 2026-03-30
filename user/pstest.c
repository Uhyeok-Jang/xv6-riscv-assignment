#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  int me = getpid();

  printf("-----ps_test: ps(0)-----\n");
  ps(0);

  printf("-----ps_test: ps(me=%d)-----\n", me);
  ps(me);

  printf("-----ps_test: ps(99999) (출력 x)-----\n");
  ps(99999);

  printf("-----ps_test: done-----\n");
  exit(0);
}