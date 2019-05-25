#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "stat.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

void print_proc_mem(const struct proc *p) {
  static char *states[] = {
          [UNUSED]    "unused",
          [EMBRYO]    "embryo",
          [SLEEPING]  "sleep ",
          [RUNNABLE]  "runble",
          [RUNNING]   "run   ",
          [ZOMBIE]    "zombie"
  };
  int i;
  char *state;
  uint pc[10];


  if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
    state = states[p->state];
  else
    state = "???";
  cprintf("%d %s %s ", p->pid, state, p->name);

  uint paged_out = (p->sz - p->res_sz) / PGSIZE;

  cprintf("%d %d %d %d %d", p->sz, paged_out, p->protected_pages, p->page_faults, p->total_paged_out);

  if(p->state == SLEEPING){
    getcallerpcs((uint*)p->context->ebp+2, pc);
    for(i=0; i<10 && pc[i] != 0; i++)
      cprintf(" %p", pc[i]);
  }
  cprintf("\n");
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->res_sz = p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

char* get_page_to_swap() {
#ifdef LIFO
  struct proc *p = myproc();
  return p->resident_pages_stack[(p->resident_pages_stack_loc--) - 1];
#endif
#ifdef SCFIFO
  struct proc *p = myproc();
  uint i = 0;
  pte_t *pte;
  uint found = 0;
  char* page = 0;

  while (!found) {
    // Get next page in the queue
    page = p->resident_pages_stack[i];

    // If the entry in the queue is empty, continue
    if (page == 0) {
      i = (i + 1) % MAX_PSYC_PAGES;
      continue;
    }

    // Find the page's pte entry
    pte = walkpgdir(p->pgdir, page, 0);

    // The page was accessed in the last time tick
    if (*pte & PTE_A) {
      // zero the accessed flag and give it a second chance
      clearpte(page, PTE_A);
      i = (i + 1) % MAX_PSYC_PAGES;
    } else {
      found = 1;
    }
  }

  // Push back all the queue from i forward
  for (; i < MAX_PSYC_PAGES - 1; ++i) {
    p->resident_pages_stack[i] = p->resident_pages_stack[i + 1];
  }

  // Set the last item in the queue to 0 - free spot
  p->resident_pages_stack[15] = 0;

  return page;
#endif
#ifdef NONE
  return 0;
#endif
}

// TODO: zero swapFilePages when swapping back in
uint get_swapfile_write_loc(char* page) {
  struct proc *p = myproc();
  int i;

  // iterate until you find a free space
  for (i = 0; p->swapFilePages[i] != 0; i++);

  p->swapFilePages[i] = page;

  return i * PGSIZE;
}

void write_to_swap(char *page) {
  struct proc *p = myproc();

  writeToSwapFile(p, page, get_swapfile_write_loc(page), PGSIZE);
  setpte(page, PTE_PG);
  clearpte(page, PTE_P);
}

void swap_out_pages(int num_pages) {
  if (num_pages <= 0) return;

  struct proc *p = myproc();

  for (int i = 0; i < num_pages; ++i) {
    char* page = get_page_to_swap();
    write_to_swap(page);
    p->res_sz -= PGSIZE;
    p->total_paged_out++;
  }
  lcr3(V2P(p->pgdir)); // flush
}

uint handle_pgflt() {
  struct proc *p = myproc();
  pte_t *pte;
  uint i;

  p->page_faults++;

  // The address that caused the page fault, and it's page
  uint addr = rcr2();
  char* page = (char*)(PGROUNDDOWN(addr));

  // Find the PTE of the address
  pte = walkpgdir(p->pgdir, (void *) addr, 0);

  // The page was not paged out
  if (*pte & PTE_PMAL) {
    p->tf->trapno = 13;
    return 0;
  }


#ifdef NONE
  return 0;
#endif

  // The page was not paged out
  if (!(*pte & PTE_PG)) return 0;

  // Set the page as present, and not paged out
  clearpte((char *) addr, PTE_PG);
  setpte((char *) addr, PTE_P);
  lcr3(V2P(p->pgdir));

  // locate page in the swap file
  for (i = 0; p->swapFilePages[i] != page && i <= MAX_PSYC_PAGES; i++);

  if (i > MAX_PSYC_PAGES)
    panic("Couldn't find page in the swap file");

  // Read from swap file into memory
  readFromSwapFile(p, page, i*PGSIZE, PGSIZE);
  p->swapFilePages[i] = 0;

  // enlarge resident size
  p->res_sz += PGSIZE;

  // Swap out another page if needed
  swap_out_pages(p->res_sz / PGSIZE - MAX_PSYC_PAGES);

#ifdef LIFO
  // Push the page to the stack of swapped in pages
  p->resident_pages_stack[p->resident_pages_stack_loc++] = page;
#endif
#ifdef SCFIFO
  // Find the first empty spot
  for (i = 0; p->resident_pages_stack[i] != 0 && i <= MAX_PSYC_PAGES; ++i);
  if (i > MAX_PSYC_PAGES) panic("handle_pgflt couldn't find free spot");
  p->resident_pages_stack[i] = page;
#endif

  return 1;
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();
  sz = curproc->sz;

#ifndef NONE
  uint overall_pages = (sz + n) / PGSIZE;
  if (overall_pages > MAX_TOTAL_PAGES) return -1;

  int pages_to_swap = (int)(curproc->res_sz + n) / PGSIZE - MAX_PSYC_PAGES; // @@@ Need to initialize res_sz
  swap_out_pages(pages_to_swap);

#endif

  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  curproc->res_sz += n;

  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->res_sz = curproc->res_sz;

  np->resident_pages_stack_loc = curproc->resident_pages_stack_loc;
  memmove(np->resident_pages_stack, curproc->resident_pages_stack, sizeof(char*)*16);
  memmove(np->swapFilePages, curproc->swapFilePages, sizeof(char*)*16);

  np->protected_pages = curproc->protected_pages;
  np->page_faults = 0;
  np->total_paged_out = 0;


  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  createSwapFile(np);

  if (curproc->swapFile) {
    struct stat st;
    filestat(curproc->swapFile, &st);
    for (i = 0; i < st.size; i += 1024) {
      char buf[1024];
      readFromSwapFile(curproc, buf, i, 1024);
      writeToSwapFile(np, buf, i, 1024);
    }
  }

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

#ifdef VERBOSE_PRINT
  print_proc_mem(curproc);
#endif

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  removeSwapFile(curproc);

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  struct proc *p;

  uint total_pages = (PHYSTOP - 4*1024*1024) / PGSIZE;
  uint free_pages = total_pages;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;

    print_proc_mem(p);

    free_pages -= p->sz / PGSIZE;
  }

  cprintf("%d / %d free pages in the system\n", free_pages, total_pages);
}
