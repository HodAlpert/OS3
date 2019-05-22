#include "param.h"
#include "types.h"
#include "user.h"

void freeze() {
  sleep(10000000);
}

void test_pmalloc() {
  // To verify that pmalloc is page aligned, and that free'ing works
  void* ptr1 = malloc(3);

  void* new_page = pmalloc();

  void* ptr2 = malloc(3);

  if (((uint)new_page-8) % PGSIZE) {
    printf(1, "Header NOT page aligned %d! FAIL\n", (uint)new_page);
  } else
    printf(1, "Header page aligned!\n");

  // The page is not protected - we should be able to write to it
  memset(new_page, 0, PGSIZE-8);

  protect_page(new_page);

  if (fork()) {
    // See that the child crashes because it accesses the protected page
    wait();
  } else {
    printf(1, "Trying to write to protected page. Expected to fail\n");
    memset(new_page, 0, 1); // Should crash
    printf(1, "Wrote to a protected page. FAIL\n");

    // Hang the child process to hang the parent
    freeze();
  }

  free(ptr1);
  free(ptr2);

  pfree(new_page);

  // The page was free'd, but it's still ours. It shouldn't be protected now
  memset(new_page, 0, PGSIZE-8);

  printf(1, "pmalloc test PASSED!\n");
}

void test_swap() {
  void* mem = malloc(PGSIZE * 14);
  memset(mem, 0, PGSIZE * 14);
  free(mem);
}

int main() {
  test_pmalloc();
//  test_swap();
  exit();
}