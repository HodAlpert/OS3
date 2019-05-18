#include <stdbool.h>
#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"

// Memory allocator by Kernighan and Ritchie,
// The C programming Language, 2nd ed.  Section 8.7.

typedef long Align;

union header {
  struct {
    union header *ptr;
    uint size;
    bool pmallocd;
  } s;
  Align x;
};

typedef union header Header;

static Header base;
static Header *freep;

void
free(void *ap)
{
  Header *bp, *p;

  bp = (Header*)ap - 1;
  for(p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
    if(p >= p->s.ptr && (bp > p || bp < p->s.ptr))
      break;
  if(bp + bp->s.size == p->s.ptr){
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  } else
    bp->s.ptr = p->s.ptr;
  if(p + p->s.size == bp){
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  } else
    p->s.ptr = bp;
  freep = p;
}

static Header*
morecore(uint nu)
{
  char *p;
  Header *hp;

  if(nu < 4096)
    nu = 4096;
  p = sbrk(nu * sizeof(Header));
  if(p == (char*)-1)
    return 0;
  hp = (Header*)p;
  hp->s.size = nu;
  free((void*)(hp + 1));
  return freep;
}

void*
inner_malloc(uint nbytes, int align)
{
  Header *p, *prevp;
  uint nunits;

  nunits = (nbytes + sizeof(Header) - 1)/sizeof(Header) + 1;
  if((prevp = freep) == 0){
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
  }
  for(p = prevp->s.ptr; ; prevp = p, p = p->s.ptr){
    if(p->s.size >= nunits){
      if(p->s.size == nunits)
        prevp->s.ptr = p->s.ptr;
      else {
        p->s.size -= nunits;
        p += p->s.size;
        p->s.size = nunits;
      }
      freep = prevp;
      p->s.pmallocd = false;
      return (void*)(p + 1);
    }
    if(p == freep)
      if((p = morecore(nunits)) == 0)
        return 0;
  }
}

/**
 * Check if p's memory, excluding the header, is aligned to a page.
 * The reason the header is excluded, is we need to be able to
 * access p's whole memory, from 0 to 4095
 */
bool aligned(void * p) {
  return !(((uint)p + sizeof(Header)) % 4096);
}

void*
malloc(uint nbytes) {
  return inner_malloc(nbytes, 0);
}

void* pmalloc() {
  Header *p, *prevp;
  uint nunits;

  nunits = (PGSIZE + sizeof(Header) - 1)/sizeof(Header) + 1;
  if((prevp = freep) == 0){
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
  }

  for(p = prevp->s.ptr; ; prevp = p, p = p->s.ptr){
    uint next_page_offset = (PGSIZE - ((uint)p) % PGSIZE) % PGSIZE;
    int found = true;

    if (p->s.size == nunits && aligned(p)) {
      // The stars have aligned - the block is exactly page-sized, and aligned.
      // No need to do anything
    } else if (aligned(p) && p->s.size > nunits) {
      // p is aligned, and it has room to put another header. Let's just return it
      Header * nextp = (void*)p + nunits; // The new next block

      nextp->s.size = p->s.size - nunits; // The new block gets new size
      nextp->s.ptr = p->s.ptr;            // The new block points like p

      prevp->s.ptr = nextp; // The prev block points to the new block

      p->s.size = nunits;
    } else if (!aligned(p)) {
      // p is not aligned. We're gonna have to split it

      if (next_page_offset < 2*sizeof(Header)) {
        // if the next page offset is too close, find the page after that
        next_page_offset += PGSIZE;
      }

      uint required_size = next_page_offset/sizeof(Header) - 1 // until the next header
              + 1                       // for the next header
              + PGSIZE/sizeof(Header)   // for the page
              + 1;                      // for the new header after the page

      // Make sure we have enough space in p for the allocated page, plus 2 new headers
      if (p->s.size < required_size) {
        found = false;
      }

      else {
        // The next page is far away - there's room to insert another header in between
        Header * nextp = (void*)p + next_page_offset - sizeof(Header);
        Header * nextnextp = (void*)p + next_page_offset + PGSIZE;

        nextnextp->s.ptr = p->s.ptr;

        uint oldsz = p->s.size;
        p->s.size = (next_page_offset - 2*sizeof(Header) - 1)/sizeof(Header) + 1;
        p->s.ptr = nextnextp;

        nextp->s.size = nunits;

        nextnextp->s.size = oldsz - p->s.size - nextp->s.size;

        prevp = p;
        p = nextp;
      }
    }

    if (found) {
      p->s.pmallocd = true;
      freep = prevp;
      return (void*)(p + 1);
    }

    if(p == freep)
      if((p = morecore(nunits)) == 0)
        return 0;
  }
}

int protect_page(void* ap) {
  Header * ph = (Header*)ap - 1;
  uint pgsize_in_headers = (PGSIZE + sizeof(Header) - 1)/sizeof(Header) + 1;

  // Verify that the address has beem pmalloc'd, and that it's the beginning of a page
  if (!ph->s.pmallocd || ph->s.size != pgsize_in_headers || (uint)ap % PGSIZE != 0) return -1;

  return 0;
}

int pfree(void* ap) {
  return 0;
}
