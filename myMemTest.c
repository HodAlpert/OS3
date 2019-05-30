#include "param.h"
#include "types.h"
#include "user.h"

#define PGSIZE 4096

void freeze() {
    sleep(10000000);
}

void test_big_malloc() {
    printf(1, "Test big malloc..\n");
    if (fork()) {
        wait();
    } else {
        void *ptr1 = malloc(21 * PGSIZE);
        memset(ptr1, 0, 21 * PGSIZE);
        free(ptr1);
        exit();
    }
    printf(1, "Test PASSED\n");
}
/**
 * verifying that pmalloc is page aligned, and that free'ing works
 */
void test_pmalloc() {
    void *ptr1 = malloc(3);

    void *new_page = pmalloc();

    void *ptr2 = malloc(3);

    if (((uint) new_page - 8) % PGSIZE) {
        printf(1, "Header NOT page aligned %d! FAIL\n", (uint) new_page - 8);
    } else
        printf(1, "Header page aligned!\n");

    // The page is not protected - we should be able to write to it
    memset(new_page, 0, PGSIZE - 8);
    protect_page(new_page);

    if (fork()) {
        // See that the child crashes because it accesses the protected page
        wait();
    } else {
        printf(1, "Trying to write to protected page. Should fail\n");
        memset(new_page, 0, 1); // Should crash
        printf(1, "Wrote to a protected page. FAIL\n");

        // Hang the child process to hang the parent
        freeze();
    }

    free(ptr1);
    free(ptr2);

    pfree(new_page);

    // The page was free'd, but it's still ours. It shouldn't be protected now
    memset(new_page, 0, PGSIZE);

    printf(1, "pmalloc test PASSED!\n");
}

void test_swap() {
    printf(1, "test swap\n");
    void *mem[1];
    for (int i = 0; i < 1; ++i) {
        if ((mem[i] = pmalloc()) == 0) {
            printf(1, "pmalloc failed\n");
            freeze();
        }
    }
    for (int i = 0; i < 1; ++i) {
        memset(mem[i], 2, PGSIZE);
    }
    for (int i = 0; i < 1; ++i) {
        pfree(mem[i]);
    }
    printf(1, "Swap test PASSED\n");
}

void test_fork() {
    printf(1, "fork test\n");
    void *mem[1];

    printf(1, "Initializing memory\n");

    for (int i = 0; i < 1; ++i) {
        if ((mem[i] = pmalloc()) == 0) {
            printf(1, "pmalloc failed\n");
            freeze();
        }
    }

    for (int i = 0; i < 1; ++i) {
        memset(mem[i], 2, PGSIZE);
    }

    printf(1, "Forking\n");
    int pid = fork();

    if (pid > 0) {
        wait();
    } else {
        printf(1, "Checking memory is the same in child process (swapped pages too)\n");
        for (int i = 0; i < 1; ++i) {
            char data = ((char *) mem[i])[0];
            if (data != 2) {
                printf(1, "memory corrupted in child process!\n");
                freeze();
            }
        }

        for (int i = 0; i < 1; ++i) {
            pfree(mem[i]);
        }
        exit();
    }

    printf(1, "Fork test PASSED\n");
}

int main() {
    test_big_malloc();
    test_pmalloc();
    test_swap();
    test_fork();
    exit();
}