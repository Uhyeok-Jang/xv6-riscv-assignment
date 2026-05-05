// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct
{
  struct spinlock lock;
  struct run *freelist;

  // freelist에 든 free physical page 개수
  int free_pages;

  // 모든 physical page에 대한 reference count
  // 일반 page는 usually 0 or 1
  // fork로 공유된 mmap page는 2 이상 될 수 있음
  int refcnt[PHYSTOP / PGSIZE];
} kmem;

// PA를 refcnt 배열 idx로 바꿈
static int
pa2idx(void *pa)
{
  return ((uint64)pa) / PGSIZE;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
  {
    // kinit -> refcnt 0
    // kfree(): refcnt 1인 page -> refcnt 0, freelist에 넣음
    // 초기 free page의 refcnt: 임시 1 -> 이후 kfree
    kmem.refcnt[pa2idx(p)] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.lock);

  int idx = pa2idx(pa);

  // refcount가 이미 0이면 free 하면 안됨
  if (kmem.refcnt[idx] < 1)
    panic("kfree: refcnt");

  // 공유 page -> refcount만 낮추고 not free(부모 자식 공유)
  kmem.refcnt[idx]--;

  if (kmem.refcnt[idx] > 0)
  {
    release(&kmem.lock);
    return;
  }

  // refcount 0 -> freelist에 넣음
  // 1로 채우는 건 for 댕글링 레퍼런스
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;
  r->next = kmem.freelist;
  kmem.freelist = r;
  kmem.free_pages++;

  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);

  r = kmem.freelist;
  if (r)
  {
    kmem.freelist = r->next;

    // freelist에서 빠져서
    kmem.free_pages--;

    // 신규 할당 page refcount: 1
    kmem.refcnt[pa2idx((void *)r)] = 1;
  }

  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk

  return (void *)r;
}

uint64
meminfo(void)
{
  uint64 bytes = 0;
  struct run *r; //freelist 탐색기

  // 다른 거 빠지면 안되니까 락
  acquire(&kmem.lock);

  // 첫 노드부터 쭉쭉 확인하면서 bytes를 더해줌
  for(r = kmem.freelist; r; r = r->next)
    bytes += PGSIZE;

  // 끝나면 락 품
  release(&kmem.lock);

  return bytes;
}

void kaddref(void *pa)
{
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kaddref");

  acquire(&kmem.lock);

  int idx = pa2idx(pa);

  // freelist의 page는 공유 대상 아님
  if (kmem.refcnt[idx] < 1)
    panic("kaddref: refcnt");

  kmem.refcnt[idx]++;

  release(&kmem.lock);
}

int freemem(void)
{
  int n;

  acquire(&kmem.lock);
  n = kmem.free_pages;
  release(&kmem.lock);

  return n;
}
