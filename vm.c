#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void) {
    struct cpu *c;

    // Map "logical" addresses to virtual addresses using identity map.
    // Cannot share a CODE descriptor for both kernel and user
    // because it would have to have DPL_USR, but the CPU forbids
    // an interrupt from CPL=0 to DPL=3.
    c = &cpus[cpuid()];
    c->gdt[SEG_KCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, 0);
    c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
    c->gdt[SEG_UCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, DPL_USER);
    c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
    lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc) {
    pde_t *pde;
    pte_t *pgtab;

    pde = &pgdir[PDX(va)];
    if (*pde & PTE_P) {
        pgtab = (pte_t *) P2V(PTE_ADDR(*pde));
    } else {
        if (!alloc || (pgtab = (pte_t *) kalloc()) == 0)
            return 0;
        // Make sure all those PTE_P bits are zero.
        memset(pgtab, 0, PGSIZE);
        // The permissions here are overly generous, but they can
        // be further restricted by the permissions in the page table
        // entries, if necessary.
        *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
    }
    return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm) {
    char *a, *last;
    pte_t *pte;

    a = (char *) PGROUNDDOWN((uint) va);
    last = (char *) PGROUNDDOWN(((uint) va) + size - 1);
    for (;;) {
        if ((pte = walkpgdir(pgdir, a, 1)) == 0)
            return -1;
        if (*pte & PTE_P)
            panic("remap");
        *pte = pa | perm | PTE_P;
        if (a == last)
            break;
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
    void *virt;
    uint phys_start;
    uint phys_end;
    int perm;
} kmap[] = {
        {(void *) KERNBASE, 0,             EXTMEM,  PTE_W}, // I/O space
        {(void *) KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
        {(void *) data,     V2P(data),     PHYSTOP, PTE_W}, // kern data+memory
        {(void *) DEVSPACE, DEVSPACE, 0,            PTE_W}, // more devices
};

/**
 * swap page_in_ram into page_in_disk, write it's data to the swap file, freeing it's memory, and writes new_virtual_memory
 * in page_in_ram
 */
void swap_page(char *new_virtual_memory, struct pages_info *page_in_disk, struct pages_info *page_in_ram);

/**
 * moving page mentions in src back from disk into dest
 * @param swapped_virtual_address: virtual address
 * @param src: page_info which we want to copy
 * @param mem: pysical memory we want to map the memory into
 * @param dest: the page_info we want to map the src into
 * @param move_memory: a boolean which indicate weather we should move the memory from the dist to the RAM
 */
void move_page_info_back_from_disk(char *swapped_virtual_address, struct pages_info *src, char *mem, struct pages_info *des);

// Set up kernel part of a page table.
pde_t *
setupkvm(void) {
    pde_t *pgdir;
    struct kmap *k;

    if ((pgdir = (pde_t *) kalloc()) == 0)
        return 0;
    memset(pgdir, 0, PGSIZE);
    if (P2V(PHYSTOP) > (void *) DEVSPACE)
        panic("PHYSTOP too high");
    for (k = kmap; k < &kmap[NELEM(kmap)];
    k++)
    if (mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                 (uint) k->phys_start, k->perm) < 0) {
        freevm(pgdir);
        return 0;
    }
    return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void) {
    kpgdir = setupkvm();
    switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void) {
    lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p) {
    if (p == 0)
        panic("switchuvm: no process");
    if (p->kstack == 0)
        panic("switchuvm: no kstack");
    if (p->pgdir == 0)
        panic("switchuvm: no pgdir");

    pushcli();
    mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                  sizeof(mycpu()->ts) - 1, 0);
    mycpu()->gdt[SEG_TSS].s = 0;
    mycpu()->ts.ss0 = SEG_KDATA << 3;
    mycpu()->ts.esp0 = (uint) p->kstack + KSTACKSIZE;
    // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
    // forbids I/O instructions (e.g., inb and outb) from user space
    mycpu()->ts.iomb = (ushort) 0xFFFF;
    ltr(SEG_TSS << 3);
    lcr3(V2P(p->pgdir));  // switch to process's address space
    popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz) {
    char *mem;

    if (sz >= PGSIZE)
        panic("inituvm: more than a page");
    mem = kalloc();
    memset(mem, 0, PGSIZE);
    mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W | PTE_U);
    memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz) {
    uint i, pa, n;
    pte_t *pte;

    if ((uint) addr % PGSIZE != 0)
        panic("loaduvm: addr must be page aligned");
    for (i = 0; i < sz; i += PGSIZE) {
        if ((pte = walkpgdir(pgdir, addr + i, 0)) == 0)
            panic("loaduvm: address should exist");
        pa = PTE_ADDR(*pte);
        if (sz - i < PGSIZE)
            n = sz - i;
        else
            n = PGSIZE;
        if (readi(ip, P2V(pa), offset + i, n) != n)
            return -1;
    }
    return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz) {
    char *mem;
    uint a;

    if (newsz >= KERNBASE)
        return 0;
    if (newsz < oldsz)
        return oldsz;

    a = PGROUNDUP(oldsz);
    for (; a < newsz; a += PGSIZE) {
        mem = kalloc();
        if (mem == 0) {
            cprintf("allocuvm out of memory\n");
            deallocuvm(pgdir, newsz, oldsz);
            return 0;
        }
#ifndef NONE
        struct proc *proc = myproc();
        if (proc->pid > 2) {
            struct pages_info *page_info = find_free_page_entry(proc->allocated_page_info);
            if (page_info) { // if there is a place to put the new page in the ram
                init_page_info(proc, (char *) a, page_info, 0);
            } else {
                struct pages_info *page_to_swap_to = find_free_page_entry(proc->swapped_pages);
                if (!page_to_swap_to) {
                    cprintf("process exceeds process memory limits\n");
                    deallocuvm(pgdir, newsz, oldsz);
                    return 0;
                }
                struct pages_info *page_in_ram = find_a_page_to_swap(proc);
                swap_page((char *) a, page_to_swap_to, page_in_ram);
            }
        }
#endif
        memset(mem, 0, PGSIZE);
        if (mappages(pgdir, (char *) a, PGSIZE, V2P(mem), PTE_W | PTE_U) < 0) {
            cprintf("allocuvm out of memory (2)\n");
            deallocuvm(pgdir, newsz, oldsz);
            kfree(mem);
            return 0;
        }
    }
    return newsz;
}

void swap_page(char *new_virtual_memory, struct pages_info *page_in_disk, struct pages_info *page_in_ram) {
    struct proc * proc = myproc();
    int index = find_index_of_page_info(proc->swapped_pages, page_in_disk);
    // writing allocated page to file
    writeToSwapFile(proc, (char *) page_in_ram->virtual_address, index * PGSIZE, PGSIZE);
    // initializing new swapped page struct
    init_page_info(proc, page_in_ram->virtual_address, page_in_disk, index);
    // clearing swapped page from memory
    pte_t *pte = walkpgdir(proc->pgdir, (char *) page_in_ram->virtual_address, 0);
    if (!check_page_flags((char *) pte, PTE_W)) {//if page is protected
        cprintf("cannot swap new_virtual_memory protected page\n"); // TODO- should we allow swapping of protected pages?
    }
    kfree((char *) P2V(PTE_ADDR(*pte)));
    *pte |= PTE_PG;
    *pte &= ~PTE_P;
    lcr3(V2P(myproc()->pgdir));
    init_page_info(proc, new_virtual_memory, page_in_ram, 0);
    proc->number_of_total_pages_out++;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz) {
    pte_t *pte;
    uint a, pa;
    struct proc *proc = myproc();

    if (newsz >= oldsz)
        return oldsz;

    a = PGROUNDUP(newsz);
    for (; a < oldsz; a += PGSIZE) {
        pte = walkpgdir(pgdir, (char *) a, 0);
        if (!pte)
            a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
        else if ((*pte & PTE_P) != 0) {
            pa = PTE_ADDR(*pte);
            if (pa == 0)
                panic("kfree");
            char *v = P2V(pa);
            kfree(v);
            if (myproc()->pid >2) {
                struct pages_info *page_info;
                if ((page_info = find_page_by_virtual_address(proc, (char *) a, proc->allocated_page_info)) != 0) {
                    page_info->allocated = 0;
                }
            }
            *pte = 0;
        }
    }
    return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir) {
    uint i;

    if (pgdir == 0)
        panic("freevm: no pgdir");
    deallocuvm(pgdir, KERNBASE, 0);
    for (i = 0; i < NPDENTRIES; i++) {
        if (pgdir[i] & PTE_P) {
            char *v = P2V(PTE_ADDR(pgdir[i]));
            kfree(v);
        }
    }
    kfree((char *) pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva) {
    pte_t *pte;

    pte = walkpgdir(pgdir, uva, 0);
    if (pte == 0)
        panic("clearpteu");
    *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    uint should_copy = 1;
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if (*pte & PTE_PG) {
        should_copy = 0;
    }
    else if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
        goto bad;
      if (should_copy) memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0)
      goto bad;
    pte = walkpgdir(d, (void *) i, 0);
    if (!should_copy) *pte &= ~PTE_P;
  }
  return d;

    bad:
    freevm(d);
    return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char *
uva2ka(pde_t *pgdir, char *uva) {
    pte_t *pte;

    pte = walkpgdir(pgdir, uva, 0);
    if ((*pte & PTE_P) == 0)
        return 0;
    if ((*pte & PTE_U) == 0)
        return 0;
    return (char *) P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len) {
    char *buf, *pa0;
    uint n, va0;

    buf = (char *) p;
    while (len > 0) {
        va0 = (uint) PGROUNDDOWN(va);
        pa0 = uva2ka(pgdir, (char *) va0);
        if (pa0 == 0)
            return -1;
        n = PGSIZE - (va - va0);
        if (n > len)
            n = len;
        memmove(pa0 + (va - va0), buf, n);
        len -= n;
        buf += n;
        va = va0 + PGSIZE;
    }
    return 0;
}

int light_page_flags(char *user_virtual_address, int flags) {
    pte_t *pte;

    pte = walkpgdir(myproc()->pgdir, user_virtual_address, 0);

    *pte |= flags;
    if (flags & PTE_W)
        myproc()->number_of_write_protected_pages--;
    lcr3(V2P(myproc()->pgdir));
    return 1;
}

int check_page_flags(char *user_virtual_address, int flags) {
    pte_t *pte;

    pte = walkpgdir(myproc()->pgdir, user_virtual_address, 0);

    return ((*pte & flags));
}

int turn_off_page_flags(char *user_virtual_address, int flags) {
    pte_t *pte;

    pte = walkpgdir(myproc()->pgdir, user_virtual_address, 0);

    *pte &= ~flags; //turn off the flag
    if (flags & PTE_W)
        myproc()->number_of_write_protected_pages++;
    lcr3(V2P(myproc()->pgdir));
    return 1;
}

void handle_page_miss(char *virtual_address) {

    struct proc *proc = myproc();

    char *swapped_virtual_address = (char *) PGROUNDDOWN((uint) virtual_address);
    struct pages_info *page_in_disk = find_page_by_virtual_address(proc, swapped_virtual_address, proc->swapped_pages);
    if (page_in_disk == 0)
        panic("could not find swapped page struct");
    char *mem = kalloc();
    if (mem == 0) {
        panic("out of memory\n");
    }
    struct pages_info *free_page = find_free_page_entry(proc->allocated_page_info);
    if (free_page) { // if there is a place to put the new page in the ram
        move_page_info_back_from_disk(swapped_virtual_address, page_in_disk, mem, free_page);
    } else {
        struct pages_info tmp_page_info;
        char tmp_page_data[PGSIZE];
        struct pages_info *page_to_replace = find_a_page_to_swap(proc);
        memmove((void *) tmp_page_data, page_to_replace->virtual_address, PGSIZE); //backing up data from RAM to tmp buffer
        cprintf("memmove moved\n");
        copy_page_info(page_to_replace, &tmp_page_info); // backing page in RAM info
        tmp_page_info.virtual_address = tmp_page_data; //assigning tmp_page with the copied data so that swap method could handle it from noe

        move_page_info_back_from_disk(swapped_virtual_address, page_in_disk, mem, page_to_replace);
        swap_page(swapped_virtual_address, page_in_disk, &tmp_page_info);
    }
}

void move_page_info_back_from_disk(char *swapped_virtual_address, struct pages_info *src, char *mem, struct pages_info *dest) {
    struct proc *proc = myproc();
    init_page_info(proc, swapped_virtual_address, dest, 0);
    if (mappages(proc->pgdir, swapped_virtual_address, PGSIZE, V2P(mem), PTE_P | PTE_W | PTE_U) < 0) {
        cprintf("could not map swapped memory back\n");
        kfree(mem);
        return;
    }
    turn_off_page_flags(swapped_virtual_address, PTE_PG);
    char page_data[PGSIZE];

    if (readFromSwapFile(proc, page_data, src->page_offset_in_swapfile, PGSIZE) < 0)
        cprintf("could not read from swap file\n");
    memmove((void *) swapped_virtual_address, page_data, PGSIZE);//moving
    init_page_info(proc, swapped_virtual_address, dest, 0); // initializing swapped back page ////why? because I said so!!!
    memset(src, 0, sizeof(struct pages_info)); // clearing old swapped page            ////why? because I said so!!!!!!
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

