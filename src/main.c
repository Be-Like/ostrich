#include <stdio.h>

const char *ostrich_greeting(void) {
    return "ostrich: ready.\n";
}

int main(void) {
    fputs(ostrich_greeting(), stdout);
    return 0;
}
