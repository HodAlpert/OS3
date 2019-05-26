#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"
#include "mmu.h"

#define PGSIZE          4096    // bytes mapped by a page

#define IS_ALIGNED(p)  !((uint)p % 4096)
#define OFFSET_UNTIL_NEXT_ALIGNED_PAGE(p) (PGSIZE - ((uint)p) % PGSIZE) % PGSIZE

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
free(void *ap) {
    Header *bp, *p;

    bp = (Header *) ap - 1;
    for (p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
        if (p >= p->s.ptr && (bp > p || bp < p->s.ptr))
            break;
    if (bp + bp->s.size == p->s.ptr) {
        bp->s.size += p->s.ptr->s.size;
        bp->s.ptr = p->s.ptr->s.ptr;
    } else
        bp->s.ptr = p->s.ptr;
    if (p + p->s.size == bp) {
        p->s.size += bp->s.size;
        p->s.ptr = bp->s.ptr;
    } else
        p->s.ptr = bp;
    freep = p;
}

/*
 * if p_flag == 1, it means the call came from pmalloc,
 * in that case, no need to touch nu
 */
static Header *
morecore(uint nu, int p_flag) {
    char *p;
    Header *hp;

    if (nu < 4096 && !p_flag)
        nu = 4096;
    p = sbrk(nu * sizeof(Header));
    if (p == (char *) -1)
        return 0;
    hp = (Header *) p;
    hp->s.size = nu;
    free((void *) (hp + 1));
    return freep;
}

void *
malloc(uint nbytes) {
    Header *p, *prevp;
    uint nunits;

    nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;
    if ((prevp = freep) == 0) {
        base.s.ptr = freep = prevp = &base;
        base.s.size = 0;
    }
    for (p = prevp->s.ptr;; prevp = p, p = p->s.ptr) {
        if (p->s.size >= nunits) {
            if (p->s.size == nunits)
                prevp->s.ptr = p->s.ptr;
            else {
                p->s.size -= nunits;
                p += p->s.size;
                p->s.size = nunits;
            }
            freep = prevp;
            return (void *) (p + 1);
        }
        if (p == freep)
            if ((p = morecore(nunits, 0)) == 0)
                return 0;
    }
}

int check_page_was_pmalloced(void *ap) {
    Header * pHeader = (Header*)ap;
    uint pgsize_in_headers = PGSIZE / sizeof(Header);
    // Verify that the address has been pmalloc'd, and that it's the beginning of a page
    if (!check_page_flags(ap, PTE_P | PTE_PMALLOCED) || pHeader->s.size != pgsize_in_headers || ((uint)ap) % PGSIZE != 0) return 0;

    return 1;
}

int protect_page(void* ap) {
    Header * pHeader = (Header*)ap - 1;
    if (!check_page_was_pmalloced(pHeader)){
        return 0;
    }
    if (turn_off_page_flags((char*)pHeader, PTE_W) < 0) {
        return 0;
    }

    return 1;
}



void * pmalloc() {
    Header *p, *prevp;
    uint nunits;

    nunits = PGSIZE  / sizeof(Header);

    if ((prevp = freep) == 0) {
        base.s.ptr = freep = prevp = &base;
        base.s.size = 0;
    }
    for (p = prevp->s.ptr;; prevp = p, p = p->s.ptr) {

        if (IS_ALIGNED(p)){
            if (p->s.size >= nunits) {
                if (p->s.size == nunits) {
                    prevp->s.ptr = p->s.ptr;
                }
                else {
                    Header * nextp = p + nunits; //assigning space for the current page
                    nextp->s.size = p->s.size - nunits; // decreasing the space pmalloced from p
                    nextp = p->s.ptr;
                    prevp->s.ptr = nextp;
                    p->s.size = nunits;
                    prevp = nextp;
                }
                freep = prevp;
                light_page_flags((char*)p, PTE_PMALLOCED);

                return (void *) (p + 1);
            }//p->s.size >= nunits
        }//IS_ALIGNED(p)
        else{// if p is not aligned
            uint required_size = OFFSET_UNTIL_NEXT_ALIGNED_PAGE(p) / sizeof(Header) // offset until next alignment
                                 + PGSIZE/sizeof(Header);   // for the page

            if (p->s.size > required_size){ //if p has enough space of the required size
                Header * aligned_page = p + ((OFFSET_UNTIL_NEXT_ALIGNED_PAGE(p) - 1)/ sizeof(Header)) + 1;
                Header * page_after_aligned_page = aligned_page + PGSIZE/sizeof(Header);

                page_after_aligned_page->s.ptr = p->s.ptr;

                uint old_size_of_p = p->s.size;
                p->s.size = ((OFFSET_UNTIL_NEXT_ALIGNED_PAGE(p) - 1)/ sizeof(Header)) + 1;
                p->s.ptr = page_after_aligned_page;

                aligned_page->s.size = nunits;
                page_after_aligned_page->s.size = old_size_of_p - p->s.size - nunits;

                prevp = p;

                light_page_flags((char*)p, PTE_PMALLOCED);

                freep = prevp;
                return (void*)(aligned_page + 1);
            }//p->s.size > required_size
            /*
             * if p has exactly the required size, it means that there is no need
             * to split p since there is already another header
             */
            else if(p->s.size == required_size){
                Header * aligned_page = p + ((OFFSET_UNTIL_NEXT_ALIGNED_PAGE(p) - 1)/ sizeof(Header)) + 1;

                p->s.size = ((OFFSET_UNTIL_NEXT_ALIGNED_PAGE(p) - 1)/ sizeof(Header)) + 1;

                aligned_page->s.size = nunits;

                prevp = p;

                light_page_flags((char*)p, PTE_PMALLOCED);
                freep = prevp;
                return (void*)(aligned_page + 1);
            }
        }
        if(p == freep)
            if((p = morecore(nunits, 1)) == 0)
                return 0;
    }
}

int pfree(void* ap){
    Header * ph = (Header*)ap - 1;
    if (!check_page_was_pmalloced(ph)) {
        return 0;
    }
    if (light_page_flags((char *) ph, PTE_W) < 0){
        return 0;
    }

    if(turn_off_page_flags((char *) ph, PTE_PMALLOCED)) return 0;
    free(ap);
    return 1;
}