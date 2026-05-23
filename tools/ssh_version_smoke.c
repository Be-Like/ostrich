#include <libssh2.h>
#include <stdio.h>

int main(void) {
    const char *ver = libssh2_version(0);
    printf("libssh2 version: %s\n", ver ? ver : "(null)");
    return ver ? 0 : 1;
}
