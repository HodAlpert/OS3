#include "param.h"
#include "types.h"
#include "user.h"

void freeze() {
  sleep(10000000);
}

int main() {
  // To verify that pmalloc is page aligned, and that free'ing works
  void* ptr1 = malloc(3);

  void* new_page = pmalloc();

  void* ptr2 = malloc(3);

  if ((uint)new_page % PGSIZE) {
    printf(1, "NOT page aligned %d! FAIL\n", (uint)new_page);
    exit();
  } else
    printf(1, "Page aligned!\n");

  // The page is not protected - we should be able to write to it
  memset(new_page, 0, PGSIZE);

  protect_page(new_page);

  if (fork()) {
    // See that the child crashes because it accesses the protected page
    wait();
  } else {
    memset(new_page, 0, 1); // Should crash
    printf(1, "Wrote to a protected page. FAIL\n");

    // Hang the child process to hang the parent
    freeze();
  }

  free(ptr1);
  free(ptr2);

  pfree(new_page);
  printf(1, "Test PASSED!\n");
  exit();
}