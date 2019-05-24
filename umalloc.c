#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"

#define PGROUNDUP(sz)  (((sz)+PGSIZE-1) & ~(PGSIZE-1))

// Memory allocator by Kernighan and Ritchie,
// The C programming Language, 2nd ed.  Section 8.7.

typedef long Align;

union header {
  struct {
    union header *ptr;
    uint size;
  } s;
  Align x;
};

typedef union header Header;

static Header base;
static Header *freep;

void
free_inner(void *ap, uint dont_merge)
{
  Header *bp, *p;

  bp = (Header*)ap - 1;
  for(p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
    if(p >= p->s.ptr && (bp > p || bp < p->s.ptr))
      break;
  if(!dont_merge && bp + bp->s.size == p->s.ptr){
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  } else
    bp->s.ptr = p->s.ptr;
  if(!dont_merge && p + p->s.size == bp){
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  } else
    p->s.ptr = bp;
  freep = p;
}

void free (void * ap) {
  free_inner(ap, 0);
}

static Header*
morecore(uint nu, uint exactly, uint dont_merge)
{
  char *p;
  Header *hp;

  if(nu < 4096 && !exactly)
    nu = 4096;
  uint bytes = nu;
  if (!exactly)
    bytes *= sizeof(Header);
  p = sbrk(bytes);
  if(p == (char*)-1)
    return 0;

  hp = (Header*)p;
  hp->s.size = exactly ? nu / sizeof(Header) : nu;
  free_inner((void*)(hp + 1), dont_merge);
  return freep;
}

void*
malloc_inner(uint nbytes, uint aligned)
{
  Header *p, *prevp;
  uint nunits;

  nunits = (nbytes + sizeof(Header) - 1)/sizeof(Header) + 1;
  if((prevp = freep) == 0){
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
  }
  for(p = prevp->s.ptr; ; prevp = p, p = p->s.ptr){
    if (aligned && (uint)p % PGSIZE != 0) continue;

    if(p->s.size >= nunits){
      if(p->s.size == nunits)
        prevp->s.ptr = p->s.ptr;
      else {
        p->s.size -= nunits;
        p += p->s.size;
        p->s.size = nunits;
      }
      freep = prevp;
      return (void*)(p + 1);
    }
    if(p == freep)
      if((p = morecore(nunits, 0, 0)) == 0)
        return 0;
  }
}

void*
malloc(uint nbytes) {
  return malloc_inner(nbytes, 0);
}

void* pmalloc() {
  if(freep == 0){
    base.s.ptr = freep = &base;
    base.s.size = 0;
  }

  uint cur_size = (uint)sbrk(0);
  uint next_page = PGROUNDUP(cur_size + sizeof(Header));
  Header * header_loc = (Header *)next_page - 1;
  uint until_next_header = (uint)header_loc - cur_size;

  morecore(until_next_header, 1, 0);

  // Now we're aligned with the next header

  morecore(PGSIZE + sizeof(Header), 1, 1);

  Header * p;
  // Find pointer to our new header
  for(p = freep; p->s.ptr != header_loc; p = p->s.ptr);
  p->s.ptr = header_loc->s.ptr;

  pmallocd(header_loc + 1, 1);
  return header_loc + 1;

}

int verify_pmallocd(Header* ph) {
  void* mem = ph + 1;
  uint pgsize_in_headers = PGSIZE/sizeof(Header) + 1;

  // Verify that the address has beem pmalloc'd, and that it's the beginning of a page
  if (!pmallocd(mem, -1) || ph->s.size != pgsize_in_headers || (uint)mem % PGSIZE != 0) return 0;

  return 1;
}

int protect_page(void* ap) {
  Header * ph = (Header*)ap - 1;
  if (!verify_pmallocd(ph)) return -1;
  protect(ap, 1);

  return 1;
}

int pfree(void* ap) {
  Header * ph = (Header*)ap - 1;
  if (!verify_pmallocd(ph)) return -1;

  if (protect(ap, 0) != 1) {
    // The page was not protected, so it wasn't pmalloc'd
    return 0;
  }

  pmallocd(ap, 0);

  free(ap);

  return 1;
}
