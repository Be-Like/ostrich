#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define ASSERT(cond, msg)                        \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL: %s\n", msg);  \
            return 1;                             \
        }                                         \
    } while (0)

int main(void) {
    FILE *p = popen("build/ostrich", "r");
    ASSERT(p != NULL, "popen(build/ostrich) failed");

    char buf[128] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    int status = pclose(p);

    ASSERT(n > 0, "no output from build/ostrich");
    ASSERT(strcmp(buf, "ostrich: ready.\n") == 0, "greeting text mismatch");
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "non-zero exit code");

    printf("PASS: smoke_test\n");
    return 0;
}
