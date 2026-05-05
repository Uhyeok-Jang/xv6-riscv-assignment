#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

// mmap_area 배열
static struct mmap_area mmap_areas[NMMAP];

// mmap_area 배열 보호 lock
static struct spinlock mmap_lock;

static struct mmap_area *find_area_by_addr_locked(struct proc *p, uint64 addr);
static struct mmap_area *find_area_containing_locked(struct proc *p, uint64 va);
static int mmap_alloc_page(struct mmap_area *ma, uint64 va);
static int mmap_read_file_page(struct mmap_area *ma, char *mem, uint64 va);
static int do_munmap(struct proc *p, uint64 addr);

void mmapinit(void)
{
    initlock(&mmap_lock, "mmap");
}

// prot 값을 PTE permission으로 바꿈
// PTE_V는 mappages 내부에서 붙음
static int
mmap_perm(int prot)
{
    int perm = PTE_U;

    if (prot & PROT_READ)
        perm |= PTE_R;

    if (prot & PROT_WRITE)
        perm |= PTE_W;

    return perm;
}

static struct mmap_area *
find_area_by_addr_locked(struct proc *p, uint64 addr)
{
    for (int i = 0; i < NMMAP; i++)
    {
        if (mmap_areas[i].p == p && mmap_areas[i].addr == addr)
            return &mmap_areas[i];
    }

    return 0;
}

static struct mmap_area *
find_area_containing_locked(struct proc *p, uint64 va)
{
    for (int i = 0; i < NMMAP; i++)
    {
        struct mmap_area *ma = &mmap_areas[i];

        if (ma->p == p &&
            ma->addr <= va &&
            va < ma->addr + ma->length)
            return ma;
    }

    return 0;
}

// file mapping -> mmap된 page에 파일 내용 읽어옴
// fileread는 f->off를 바꿈 -> mmap의 offset 의미와 다소 상이 -> 사용 x
//
static int
mmap_read_file_page(struct mmap_area *ma, char *mem, uint64 va)
{
    if (ma->f == 0)
        return -1;

    if (ma->f->type != FD_INODE)
        return -1;

    // virtual page가 mapping 시작점에서 떨어져 있는 바이트 수 계산
    uint file_offset = ma->offset + (uint)(va - ma->addr);

    ilock(ma->f->ip);

    // user_dst = 0 -> dst는 kernel address
    // if PGSIZE > 파일 내용 -> readi는 적게 읽음, 나머지는 mmap_alloc_page에서 0으로 초기화해 둔 상태로 남음
    int n = readi(ma->f->ip, 0, (uint64)mem, file_offset, PGSIZE);

    iunlock(ma->f->ip);

    if (n < 0)
        return -1;

    return 0;
}

// ma의 virtual page 하나당 physical page 할당, page table에 연결
// 헬퍼 for MAP_POPULATE, lazy page fault handler
static int
mmap_alloc_page(struct mmap_area *ma, uint64 va)
{
    struct proc *p = ma->p;
    char *mem;
    pte_t *pte;
    int perm;

    va = PGROUNDDOWN(va);

    // already mapped -> 중복 mappages() x
    pte = walk(p->pagetable, va, 0);
    if (pte && (*pte & PTE_V))
        return 1;

    mem = kalloc();
    if (mem == 0)
        return -1;

    // anonymous mapping -> 0으로 채워진 page
    // file mapping -> 파일 크기보다 뒤쪽은 0 -> 일단 0 초기화
    memset(mem, 0, PGSIZE);

    if ((ma->flags & MAP_ANONYMOUS) == 0)
    {
        if (mmap_read_file_page(ma, mem, va) < 0)
        {
            kfree(mem);
            return -1;
        }
    }

    perm = mmap_perm(ma->prot);

    if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) < 0)
    {
        kfree(mem);
        return -1;
    }

    return 1;
}

// kernel mmap
uint64
kmmap(uint64 addr, int length, int prot, int flags, int fd, int offset)
{
    struct proc *p = myproc();
    struct file *f = 0;
    struct mmap_area *ma = 0;
    uint64 start;

    // addr가 page boundary에 align
    if (addr % PGSIZE != 0)
        return 0;

    // length가 PGSIZE의 배수
    if (length <= 0 || length % PGSIZE != 0)
        return 0;

    if (prot != PROT_READ && prot != (PROT_READ | PROT_WRITE))
        return 0;

    if (flags & ~(MAP_ANONYMOUS | MAP_POPULATE))
        return 0;

    start = MMAPBASE + addr;

    if (flags & MAP_ANONYMOUS)
    {
        // anonymous mapping: file descriptor x
        if (fd != -1 || offset != 0)
            return 0;
    }
    else
    {
        // file mapping: fd 필수
        if (fd < 0 || fd >= NOFILE)
            return 0;

        f = p->ofile[fd];
        if (f == 0)
            return 0;

        if (f->type != FD_INODE)
            return 0;

        // prot, file open flag 맞아야함
        if ((prot & PROT_READ) && !f->readable)
            return 0;

        if ((prot & PROT_WRITE) && !f->writable)
            return 0;
    }

    // 빈 mmap_area entry 찾아서 등록
    acquire(&mmap_lock);

    for (int i = 0; i < NMMAP; i++)
    {
        if (mmap_areas[i].p == 0)
        {
            ma = &mmap_areas[i];
            break;
        }
    }

    if (ma == 0)
    {
        release(&mmap_lock);
        return 0;
    }

    ma->f = f ? filedup(f) : 0;
    ma->addr = start;
    ma->length = length;
    ma->offset = offset;
    ma->prot = prot;
    ma->flags = flags;
    ma->p = p;

    release(&mmap_lock);

    // MAP_POPULATE -> mmap 시점에 전체 page 즉시 할당
    if (flags & MAP_POPULATE)
    {
        for (uint64 va = start; va < start + length; va += PGSIZE)
        {
            if (mmap_alloc_page(ma, va) < 0)
            {
                // 중간에 실패 시 이미 만든 거 정리
                do_munmap(p, start);
                return 0;
            }
        }
    }

    return start;
}

int kmunmap(uint64 addr)
{
    return do_munmap(myproc(), addr);
}

// for sys_munmap, exit cleanup
static int
do_munmap(struct proc *p, uint64 addr)
{
    struct mmap_area local;
    struct mmap_area *ma;
    pte_t *pte;

    if (addr % PGSIZE != 0)
        return -1;

    // mmap_area entry 찾음 -> 전역 배열 entry 비움
    // page table 정리, fileclose는 lock없이
    acquire(&mmap_lock);

    ma = find_area_by_addr_locked(p, addr);
    if (ma == 0)
    {
        release(&mmap_lock);
        return -1;
    }

    local = *ma;
    memset(ma, 0, sizeof(*ma));

    release(&mmap_lock);

    // lazy mapping: 일부 page만 실제 할당일 수도 있음
    // 각 page PTE 있는지 확인해서 있는 page만 해제
    for (uint64 va = local.addr; va < local.addr + local.length; va += PGSIZE)
    {
        pte = walk(p->pagetable, va, 0);

        if (pte == 0)
            continue;

        if ((*pte & PTE_V) == 0)
            continue;

        uint64 pa = PTE2PA(*pte);

        // kfree는 refcount-aware
        // 공유 page -> refcount만 감소
        // 마지막 mapping 사라질 때만 freelist로 돌아감
        kfree((void *)pa);

        *pte = 0;
    }

    // page table 변경하고 flush
    sfence_vma();

    if (local.f)
        fileclose(local.f);

    return 1;
}

// mmap page fault handler called in trap
// write_fault가 1 -> store page fault
// write_fault가 0 -> load page fault
int mmap_handle_pagefault(uint64 faultva, int write_fault)
{
    struct proc *p = myproc();
    struct mmap_area local;
    struct mmap_area *ma;
    uint64 va = PGROUNDDOWN(faultva);

    acquire(&mmap_lock);

    ma = find_area_containing_locked(p, va);
    if (ma == 0)
    {
        release(&mmap_lock);
        return -1;
    }

    local = *ma;

    release(&mmap_lock);

    // PROT_READ에 write -> 실패 
    if (write_fault && ((local.prot & PROT_WRITE) == 0))
        return -1;

    return mmap_alloc_page(&local, va);
}

// 현재 process의 모든 mmap_area 정리
// called by exit
void mmap_cleanup(struct proc *p)
{
    uint64 addr;

    for (;;)
    {
        addr = 0;

        acquire(&mmap_lock);

        for (int i = 0; i < NMMAP; i++)
        {
            if (mmap_areas[i].p == p)
            {
                addr = mmap_areas[i].addr;
                break;
            }
        }

        release(&mmap_lock);

        if (addr == 0)
            break;

        do_munmap(p, addr);
    }
}

// fork: copy parent mmap_area to child (같은 PA 할당)
int mmap_fork(struct proc *parent, struct proc *child)
{
    struct mmap_area list[NMMAP];
    int n = 0;

    acquire(&mmap_lock);

    for (int i = 0; i < NMMAP; i++)
    {
        if (mmap_areas[i].p == parent)
        {
            if (n >= NMMAP)
            {
                release(&mmap_lock);
                mmap_cleanup(child);
                return -1;
            }

            struct mmap_area *dst = 0;

            for (int j = 0; j < NMMAP; j++)
            {
                if (mmap_areas[j].p == 0)
                {
                    dst = &mmap_areas[j];
                    break;
                }
            }

            if (dst == 0)
            {
                release(&mmap_lock);
                mmap_cleanup(child);
                return -1;
            }

            *dst = mmap_areas[i];
            dst->p = child;

            if (dst->f)
                dst->f = filedup(dst->f);

            list[n++] = *dst;
        }
    }

    release(&mmap_lock);

    // 실제 할당된 거만 child에게도 map
    for (int i = 0; i < n; i++)
    {
        struct mmap_area *ma = &list[i];

        for (uint64 va = ma->addr; va < ma->addr + ma->length; va += PGSIZE)
        {
            pte_t *pte = walk(parent->pagetable, va, 0);

            if (pte == 0)
                continue;

            if ((*pte & PTE_V) == 0)
                continue;

            uint64 pa = PTE2PA(*pte);
            uint flags = PTE_FLAGS(*pte);

            // 같은 physical page 가리키게 refcount 증가
            kaddref((void *)pa);

            if (mappages(child->pagetable, va, PGSIZE, pa, flags) < 0)
            {
                // mappages 실패 -> refcount 복구
                kfree((void *)pa);

                mmap_cleanup(child);
                return -1;
            }
        }
    }

    return 0;
}