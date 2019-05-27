// Per-CPU state
#define MAX_PSYC_PAGES 16
#define MAX_TOTAL_PAGES 32

struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

struct pages_info {
    volatile int allocated;                  // is the current page allocated
    char * virtual_address;         // page's virtual address
    pde_t* pgdir;                   // Page table
    uint page_offset_in_swapfile;   // page's offset in the swapfile (if swapped)
    uint creation_time;              // for FIFO and LIFO
};


enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  uint res_sz;                 // Size of memory resident in physical RAM
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  uint time;                   // for LIFO and FIFO selection
  //Swap file. must initiate with create swap file
  struct file *swapFile;      //page file
  struct pages_info allocated_page_info[MAX_PSYC_PAGES];
  struct pages_info swapped_pages[MAX_PSYC_PAGES];
  int number_of_total_pages_out;
  int number_of_write_protected_pages;
  int number_of_PGFLT;
};
/**
 * searches proc->allocated_page_info entry's and looking for a non allocated entry.
 * @param pages_info_table: page_info entry, could be the allocated or the swapped entry of a given process
 * @return the address of a pages_info which is not allocated at the moment if there is one
 *         otherwise: return 0.
 */
struct pages_info * find_free_page_entry(struct pages_info * pages_info_table);

/**
 *
 * @param proc: process to swap pages in
 * @return pointer to the page which will be swapped
 */
struct pages_info * find_a_page_to_swap(struct proc * proc);


/**
 *
 * @param proc: process in which the page should be initialized
 * @param a: virtual address of the page
 * @param page: pages_info entry of the place where the page will be swapped to
 * @param index: index of swapped pages entry to init thr page_info in
 */
void init_page_info(struct proc *proc, char* a, struct pages_info *page, int index);

struct pages_info *
find_page_by_virtual_address(char *a, struct pages_info *page_info_array, pde_t *pgdir);

/**
 * returns index of page_info_requested in pages_info_table
 */
int find_index_of_page_info(struct pages_info *pages_info_table, struct pages_info *page_info_requested);

/**
 * copying page_info from src to dest
 */
void copy_page_info(struct pages_info *src, struct pages_info *dest, pte_t *pgdir);
void update_new_page_info_array(struct proc *np, struct proc *curproc, pte_t *pgdir);
struct pages_info *find_page_by_LIFO(struct proc *proc);
struct pages_info *find_page_by_SCFIFO(struct proc *proc);
// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
