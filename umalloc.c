#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"
#define PGSIZE          4096    // bytes mapped by a page
#define PGROUNDUP(sz)  (((sz)+PGSIZE-1) & ~(PGSIZE-1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE-1))

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

int verify_pmallocd(void* ap) {
    Header * ph = (Header*)ap - 1;
    uint pgsize_in_headers = (PGSIZE + sizeof(Header) - 1)/sizeof(Header) + 1;

    // Verify that the address has beem pmalloc'd, and that it's the beginning of a page
    if (!ph->s.pmallocd || ph->s.size != pgsize_in_headers || (uint)ap % PGSIZE != 0) return 0;

    return 1;
}

int protect_page(void* ap) {
    if (!check_pmallocked_bit(ap)) return -1;
    protect(ap, 1);

    return 1;
}

void * pmalloc() {
    Header *p, *prevp;
    uint nunits;

    nunits = (PGSIZE + sizeof(Header) - 1) / sizeof(Header) + 1;
    if ((prevp = freep) == 0) {
        base.s.ptr = freep = prevp = &base;
        base.s.size = 0;
    }
    for (p = prevp->s.ptr;; prevp = p, p = p->s.ptr) {
        if (IS_ALIGNED(p)){
            if (p->s.size >= nunits) {
                if (p->s.size == nunits)
                    prevp->s.ptr = p->s.ptr;
                else {
                    p->s.size -= nunits;
                    p += p->s.size;
                    p->s.size = nunits;
                }
                freep = prevp;
                //TODO light the bit
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

                //TODO light the bit
                freep = prevp;
                return (void*)(aligned_page + 1);
            }//p->s.size > required_size
            /*
             * if p has exactly the required size, it means that there is no need
             * to split p since there is already another header
             */
            else if(p->s.size == required_size){
                Header * aligned_page = p + ((OFFSET_UNTIL_NEXT_ALIGNED_PAGE(p) - 1)/ sizeof(Header)) + 1;


                uint old_size_of_p = p->s.size;
                p->s.size = ((OFFSET_UNTIL_NEXT_ALIGNED_PAGE(p) - 1)/ sizeof(Header)) + 1;

                aligned_page->s.size = nunits;

                prevp = p;

                //TODO light the bit
                freep = prevp;
                return (void*)(aligned_page + 1);
            }
        }
        if(p == freep)
            if((p = morecore(nunits)) == 0)
                return 0;
    }
}
