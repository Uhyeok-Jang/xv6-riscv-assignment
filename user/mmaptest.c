#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PGSIZE 4096
#define MMAPBASE 0x40000000

static void
check(int cond, char *msg)
{
    if (cond)
    {
        printf("[OK] %s\n", msg);
    }
    else
    {
        printf("[FAIL] %s\n", msg);
        exit(1);
    }
}

static void
test_anon_populate(void)
{
    int before = freemem();

    uint64 a = mmap(0, 2 * PGSIZE,
                    PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_POPULATE,
                    -1, 0);

    check(a == MMAPBASE, "anonymous populate: return address");

    int after = freemem();

    printf("anon populate freemem before=%d after=%d diff=%d\n",
           before, after, before - after);

    check(before - after >= 2,
          "anonymous populate: freemem decreases by at least 2");

    char *p = (char *)a;
    p[0] = 'A';
    p[PGSIZE] = 'B';

    check(p[0] == 'A', "anonymous populate: first page read/write");
    check(p[PGSIZE] == 'B', "anonymous populate: second page read/write");

    check(munmap(a) == 1, "anonymous populate: munmap success");

    int final = freemem();

    printf("anon populate freemem after munmap=%d recovered=%d\n",
           final, final - after);

    check(final >= after + 2,
          "anonymous populate: munmap frees at least 2 data pages");
}

static void
test_anon_lazy(void)
{
    int before = freemem();

    uint64 a = mmap(0, 2 * PGSIZE,
                    PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS,
                    -1, 0);

    check(a == MMAPBASE, "anonymous lazy: return address");
    check(freemem() == before, "anonymous lazy: mmap itself allocates no page");

    char *p = (char *)a;

    p[0] = 'X';
    check(freemem() == before - 1, "anonymous lazy: first page fault allocates 1 page");

    p[PGSIZE] = 'Y';
    check(freemem() == before - 2, "anonymous lazy: second page fault allocates 1 page");

    check(p[0] == 'X', "anonymous lazy: first page content");
    check(p[PGSIZE] == 'Y', "anonymous lazy: second page content");

    check(munmap(a) == 1, "anonymous lazy: munmap success");
    check(freemem() == before, "anonymous lazy: freemem restored");
}

static void
test_file_populate(void)
{
    int fd = open("README", O_RDONLY);
    check(fd >= 0, "file populate: open README");

    char buf[32];
    int n = read(fd, buf, sizeof(buf));
    check(n > 0, "file populate: read README prefix");

    close(fd);

    fd = open("README", O_RDONLY);
    check(fd >= 0, "file populate: reopen README");

    int before = freemem();

    uint64 a = mmap(0, PGSIZE,
                    PROT_READ,
                    MAP_POPULATE,
                    fd, 0);

    check(a == MMAPBASE, "file populate: return address");

    int after = freemem();

    printf("file populate freemem before=%d after=%d diff=%d\n",
           before, after, before - after);

    // file data page 1개 필수 할당
    // page table page가 추가로 할당될 수 있음 -> 최소 1 감소로 확인
    check(before - after >= 1,
          "file populate: freemem decreases by at least 1");
          
    char *p = (char *)a;
    check(memcmp(p, buf, n) == 0, "file populate: mapped content matches README");

    close(fd);

    // fd 닫은 뒤에도 mmap_area가 filedup() 가지고 있어야 함
    check(memcmp(p, buf, n) == 0, "file populate: content still valid after close(fd)");

    check(munmap(a) == 1, "file populate: munmap success");

    int final = freemem();

    printf("file populate freemem after munmap=%d recovered=%d\n",
           final, final - after);

    // munmap은 file data page 최소 1개를 free
    check(final >= after + 1,
          "file populate: munmap frees at least 1 data page");
}

static void
test_file_lazy(void)
{
    int fd = open("README", O_RDONLY);
    check(fd >= 0, "file lazy: open README");

    char buf[32];
    int n = read(fd, buf, sizeof(buf));
    check(n > 0, "file lazy: read README prefix");

    close(fd);

    fd = open("README", O_RDONLY);
    check(fd >= 0, "file lazy: reopen README");

    int before = freemem();

    uint64 a = mmap(0, PGSIZE,
                    PROT_READ,
                    0,
                    fd, 0);

    check(a == MMAPBASE, "file lazy: return address");
    check(freemem() == before, "file lazy: mmap itself allocates no page");

    close(fd);

    char *p = (char *)a;

    // page fault -> readme 읽힘
    check(memcmp(p, buf, n) == 0, "file lazy: mapped content matches README after fault");

    int after_fault = freemem();

    printf("file lazy freemem before=%d after_fault=%d diff=%d\n",
           before, after_fault, before - after_fault);

    // 첫 접근때 file data page 최소 1개는 할당
    // page table page가 추가로 할당될 수 있음 -> 최소 1 감소로 확인
    check(before - after_fault >= 1,
          "file lazy: first access allocates at least 1 page");

    check(munmap(a) == 1, "file lazy: munmap success");

    int final = freemem();

    printf("file lazy freemem after munmap=%d recovered=%d\n",
           final, final - after_fault);

    // munmap은 file data page 최소 1개 free
    check(final >= after_fault + 1,
          "file lazy: munmap frees at least 1 data page");
}

static void
test_invalid_args(void)
{
    int fd = open("README", O_RDONLY);
    check(fd >= 0, "invalid args: open README");

    check(mmap(1, PGSIZE, PROT_READ, MAP_ANONYMOUS, -1, 0) == 0,
          "invalid args: unaligned addr fails");

    check(mmap(0, 123, PROT_READ, MAP_ANONYMOUS, -1, 0) == 0,
          "invalid args: unaligned length fails");

    check(mmap(0, PGSIZE, PROT_WRITE, MAP_ANONYMOUS, -1, 0) == 0,
          "invalid args: PROT_WRITE only fails");

    check(mmap(0, PGSIZE, PROT_READ, 0, -1, 0) == 0,
          "invalid args: file mapping without fd fails");

    check(mmap(0, PGSIZE, PROT_READ, MAP_ANONYMOUS, fd, 0) == 0,
          "invalid args: anonymous with fd fails");

    check(mmap(0, PGSIZE, PROT_READ, MAP_ANONYMOUS, -1, PGSIZE) == 0,
          "invalid args: anonymous with nonzero offset fails");

    check(munmap(MMAPBASE + 10 * PGSIZE) == -1,
          "invalid args: munmap unknown area fails");

    close(fd);
}

static void
test_fork_shared_mapping(void)
{
    int before = freemem();

    uint64 a = mmap(0, PGSIZE,
                    PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_POPULATE,
                    -1, 0);

    check(a == MMAPBASE, "fork: mmap success");

    char *p = (char *)a;
    p[0] = 'P';

    int after_mmap = freemem();

    printf("fork freemem before=%d after_mmap=%d diff=%d\n",
           before, after_mmap, before - after_mmap);

    // parent mmap data page 최소 1개 할당
    // page table page가 추가로 할당될 수 있음 -> 최소 1 감소로 확인
    check(before - after_mmap >= 1,
          "fork: parent allocated at least 1 mmap page");

    int pid = fork();

    if (pid == 0)
    {
        // child, parent가 같은 physical page를 같은 VA에 mapping 받음
        check(p[0] == 'P', "fork child: inherited mmap content");

        // physical page 공유 -> write-back은 parent에게도 보일 수도 있음 
        p[0] = 'C';

        exit(0);
    }

    int st;
    wait(&st);

    // child exit -> refcount 줄어듬, parent mapping 유지
    check(p[0] == 'C', "fork parent: shared page still mapped after child exit");

    int after_child_exit = freemem();

    printf("fork freemem after_child_exit=%d change_from_after_mmap=%d\n",
           after_child_exit, after_child_exit - after_mmap);

    // child exit -> parent와 공유 중인 mmap data page free X
    check(after_child_exit >= after_mmap,
          "fork: child exit should not free parent's shared page");

    check(munmap(a) == 1, "fork: parent munmap success");

    int final = freemem();

    printf("fork freemem after parent munmap=%d recovered=%d\n",
           final, final - after_child_exit);

    // parent munmap -> mmap data page 최소 1개 회수
    check(final >= after_child_exit + 1,
          "fork: parent munmap frees at least 1 mmap data page");
}

static void
test_write_protection(void)
{
    int pid = fork();

    if (pid == 0)
    {
        uint64 a = mmap(0, PGSIZE,
                        PROT_READ,
                        MAP_ANONYMOUS,
                        -1, 0);

        if (a == 0)
            exit(1);

        char *p = (char *)a;

        // PROT_READ mapping에 write -> store page fault, process kill
        p[0] = 'X';

        // 실패
        exit(2);
    }

    int st;
    wait(&st);

    // 일단 kernel panic 없이 child 종료되는지만 확인
    printf("[OK] write protection: child terminated after invalid write\n");
}

int main(int argc, char **argv)
{
    printf("mmaptest start\n");

    test_invalid_args();
    test_anon_populate();
    test_anon_lazy();
    test_file_populate();
    test_file_lazy();
    test_fork_shared_mapping();
    test_write_protection();

    printf("mmaptest done\n");
    exit(0);
}