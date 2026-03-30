#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  uint64 before = meminfo();
  printf("test; free memory: %lu bytes\n", before);

 // Optional sanity check: allocate one page eagerly and check meminfo again.
  char *p = sbrk(4096);
  if(p == SBRK_ERROR){
    printf("test; sbrk 실패\n");
    exit(1);
  }
  // Touch it (harmless; ensures it's mapped/used from user side as well).
  p[0] = 1;

  uint64 after = meminfo();
  printf("test; sbrk(4096) 이후: %lu bytes\n", after);
  if(after <= before)
    printf("test; free decreased by ~%lu bytes\n", before - after);
  else
    printf("test; free increased by %lu bytes (타 frees에 따라 가능할 수도)\n", after - before);

  exit(0);
}
