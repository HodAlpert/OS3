#include "param.h"
#include "types.h"
#include "user.h"

int main() {
  void* new_page = pmalloc();
  memset(new_page, 2, PGSIZE);
  protect_page(new_page);
  memset(new_page, 2, PGSIZE); // Should fail
  pfree(new_page);
  printf(1, "Test PASSED!\n");
  exit();
}