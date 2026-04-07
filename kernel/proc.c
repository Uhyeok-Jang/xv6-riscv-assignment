#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// nice 값에 대응하는 weight 배열
const int weight_array[40] = 
{
  88818, 71054, 56843, 45475, 36380, 29104, 23283, 18626, 14901, 11921,
  9537, 7629, 6104, 4883, 3906, 3125, 2500, 2000, 1600, 1280,
  1024, 819, 655, 524, 419, 336, 268, 215, 172, 137,
  110, 88, 70, 56, 45, 36, 29, 23, 18, 15,
};

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // 파라미터 초기화
  p->nice = 20;
  p->runtime = 0;
  p->vruntime = 0;
  p->time_slice = 5;
  p->total_tick = 0;

  //초기 데드라인
  int weight = weight_array[p->nice];
  p->vdeadline = (5000 * 1024) / weight;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  // fork될 때 상속돼야함
  np->nice = p->nice;
  np->vruntime = p->vruntime;

  int weight = weight_array[np->nice];
  np->vdeadline = np->vruntime + (5000 * 1024) / weight;

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

int waitpid(int pid)
{
  struct proc *pp; // 탐색할 프로세스들을 담을 변수 (자식 후보들)
  struct proc *p = myproc(); // 나 자신 (현재 실행 중인 부모 프로세스)

  // 상속 관계 얽히니까 락
  acquire(&wait_lock); 

  //자식이 죽을 때까지 루프
  for (;;)
  {
    // 전체 프로세스 테이블을 훑음
    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
      // 내가 부모고, pid 일치하면
      if (pp->parent == p && pp->pid == pid)
      {
        // 자식의 상태를 확인하기 위해 자식에게 락을 걺
        acquire(&pp->lock);

        // 좀비라면? 
        if (pp->state == ZOMBIE)
        {
          //자원 수거 -> 자식, 전체 대기 락 해제 -> 
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return 0;
        }
        //살아있으면 자식 락 품
        release(&pp->lock);

        //부모 kill 당하면
        if (killed(p))
        {
          release(&wait_lock);
          return -1;
        }
        //부모는 자식 끝날 때까지 루프 돌면서 대기
        sleep(p, &wait_lock);
        goto continue_wait;
      }
    }

    // 다 돌았는데 pid 없으면 에러
    release(&wait_lock);
    return -1;

  continue_wait:;
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    // min_vruntime, 전체 weight 합 구해야함
    uint64 min_v = (uint64)-1;
    uint64 w_sum = 0;
    int is_runnable = 0; // 실행할 프로세스 있는지 플래그

    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE || p->state == RUNNING)
      {
        // 런큐 내 min vruntime 찾음
        if (p->vruntime < min_v)
        {
          min_v = p->vruntime;
        }

        // 런큐 가중치합 누적
        w_sum += weight_array[p->nice];

        if (p->state == RUNNABLE)
          is_runnable = 1;
      }
      release(&p->lock);
    }

    // 실행 가능 없으면 wfi
    if (is_runnable == 0)
    {
      asm volatile("wfi");
      continue;
    }

    // eligibility 판단 좌항
    uint64 left_side = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE || p->state == RUNNING)
      {
        // 좌항 공식
        left_side += (p->vruntime - min_v) * weight_array[p->nice];
      }
      release(&p->lock);
    }

    // eligible, vdead 빠른 프로세스 찾기
    struct proc *best_p = 0;
    uint64 best_vdeadline = (uint64)-1;

    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        // 우항 공식
        uint64 right_side = (p->vruntime - min_v) * w_sum;

        // eligible 판단
        int is_eligible = (left_side >= right_side);

        if (is_eligible && p->vdeadline < best_vdeadline)
        {
          best_p = p;
          best_vdeadline = p->vdeadline;
        }
      }
      release(&p->lock);
    }

    // 선택 프로세스 컨텍스트 교환
    if (best_p)
    {
      // 락 잡고 상태 재확인
      acquire(&best_p->lock);
      if (best_p->state == RUNNABLE)
      {
        best_p->state = RUNNING;
        c->proc = best_p;

        swtch(&c->context, &best_p->context);

        c->proc = 0;
      }
      release(&best_p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        // 다시 실행 가능 상태로 변경
        p->state = RUNNABLE;

        // Wake up 시퀀스 업데이트
        p->time_slice = 5;

        int weight = weight_array[p->nice];
        p->vdeadline = p->vruntime + (5000 * 1024) / weight;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// getnice added
int getnice(int pid)
{
  struct proc *p;

  // 테이블 뒤져서 해당 프로세스 찾음
  for (p = proc; p < &proc[NPROC]; p++)
  {
    //락 걸고 해
    acquire(&p->lock);
    // 사용 중이고, pid 맞으면?
    if (p->state != UNUSED && p->pid == pid)
    {
      int v = p->nice; //값 복사
      release(&p->lock); //락 해제
      return v;
    }
    release(&p->lock); //락 해제
  }
  return -1;
}

// setnice added
int setnice(int pid, int value)
{
  struct proc *p;

  if (value < NICE_MIN || value > NICE_MAX)
    return -1;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state != UNUSED && p->pid == pid)
    {
      p->nice = value;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// 전체 시스템 ticks
extern uint ticks;

// ps system call added
void ps(int pid)
{
  // 각 state name
  static char *states[] = {
      [UNUSED] "UNUSED  ",
      [USED] "USED    ",
      [SLEEPING] "SLEEPING",
      [RUNNABLE] "RUNNABLE",
      [RUNNING] "RUNNING ",
      [ZOMBIE] "ZOMBIE  ",
  };

  struct proc *p;

  // 런큐 전체 순회 -> eligibility 판단 위한 전역값 계산
  uint64 min_v = (uint64)-1;
  uint64 w_sum = 0;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == RUNNING || p->state == RUNNABLE)
    {
      if (p->vruntime < min_v)
        min_v = p->vruntime;
      w_sum += weight_array[p->nice];
    }
    release(&p->lock);
  }

  uint64 left_side = 0;
  if (min_v != (uint64)-1)
  {
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNING || p->state == RUNNABLE)
      {
        left_side += (p->vruntime - min_v) * weight_array[p->nice];
      }
      release(&p->lock);
    }
  }

  // 헤더
  printf("name           \tpid\tstate   \tpriority\truntime/weight\truntime\t\tvruntime\tvdeadline\tis_eligible\ttick %d\n", ticks*1000);

  // 프로세스 테이블 순회
  for (p = proc; p < &proc[NPROC]; p++)
  {
    int curpid, curnice, eligible_flag = 0;
    uint64 cur_runtime, cur_vruntime, cur_vdeadline;
    enum procstate st;
    char name[16];

    // 락 for 정보 추출
    acquire(&p->lock);

    if (p->state == UNUSED)
    {
      release(&p->lock);
      continue;
    }
    if (pid != 0 && p->pid != pid)
    {
      release(&p->lock);
      continue;
    }

    // snapshot
    st = p->state;
    curpid = p->pid;
    curnice = p->nice;
    cur_runtime = p->runtime;
    cur_vruntime = p->vruntime;
    cur_vdeadline = p->vdeadline;
    safestrcpy(name, p->name, sizeof(name));

    // eligibility 판단
    if (st == RUNNING || st == RUNNABLE)
    {
      uint64 right_side = (cur_vruntime - min_v) * w_sum;
      if (left_side >= right_side)
        eligible_flag = 1;
    }

    release(&p->lock);

    // 상태 번호 바꾸기
    char *state = "???";
    if (st >= 0 && st < NELEM(states) && states[st])
      state = states[st];

    // runtime/weight 계산
    int weight = weight_array[curnice];
    int rtime_w = cur_runtime / weight;

    char padded_name[16];
    safestrcpy(padded_name, name, sizeof(padded_name));
    int len = 0;
    while (padded_name[len] != '\0')
      len++;
    while (len < 15)
    {
      padded_name[len++] = ' ';
    }
    padded_name[15] = '\0';

    char *eligible_str = eligible_flag ? "true " : "false";

    // 최종 출력
    printf(
      "%s\t%d\t%s\t%d\t\t%d\t\t%d\t\t%d\t\t%d\t\t%s\n",
      padded_name, curpid, state, curnice, rtime_w,
      (int)cur_runtime, (int)cur_vruntime, (int)cur_vdeadline, eligible_str
    );

    // 특정 프로세스 여부
    if (pid != 0)
      return;
  }
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
