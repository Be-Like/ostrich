#include "../include/lexicon.h"
#include <stdio.h>
#include <string.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

static int test_all_keys_non_empty(void) {
    for (int k = 0; k < LEX__COUNT; k++) {
        const char *s = lex((LexKey)k);
        ASSERT("key resolves to non-NULL", s != NULL);
        ASSERT("key resolves to non-empty string", s[0] != '\0');
    }
    PASS("all_keys_non_empty");
    return 0;
}

static int test_identity_string(void) {
    const char *s = lex(LEX_IDENTITY);
    ASSERT("identity is non-NULL", s != NULL);
    ASSERT("identity contains OSTRICH", strstr(s, "OSTRICH") != NULL);
    ASSERT("identity contains infiltration console",
           strstr(s, "infiltration console") != NULL);
    PASS("identity_string");
    return 0;
}

static int test_voice_prefix(void) {
    const char *s = lex(LEX_VOICE_PREFIX);
    ASSERT("voice prefix is non-NULL", s != NULL);
    ASSERT("voice prefix is >", strcmp(s, ">") == 0);
    PASS("voice_prefix");
    return 0;
}

static int test_footer_name(void) {
    const char *s = lex(LEX_FOOTER_NAME);
    ASSERT("footer name is non-NULL", s != NULL);
    ASSERT("footer name is ostrich", strcmp(s, "ostrich") == 0);
    PASS("footer_name");
    return 0;
}

static int test_footer_online(void) {
    const char *s = lex(LEX_FOOTER_ONLINE);
    ASSERT("footer online is non-NULL", s != NULL);
    ASSERT("footer online is ONLINE", strcmp(s, "ONLINE") == 0);
    PASS("footer_online");
    return 0;
}

static int test_wordmark_non_empty(void) {
    const char *s = lex(LEX_WORDMARK);
    ASSERT("wordmark is non-NULL", s != NULL);
    ASSERT("wordmark is non-empty", s[0] != '\0');
    PASS("wordmark_non_empty");
    return 0;
}

static int test_out_of_range_stable(void) {
    /* just past the last valid key */
    const char *s = lex(LEX__COUNT);
    ASSERT("out-of-range returns non-NULL", s != NULL);
    ASSERT("out-of-range returns non-empty placeholder", s[0] != '\0');

    /* a large value should also return a stable placeholder */
    const char *s2 = lex((LexKey)9999);
    ASSERT("large out-of-range returns non-NULL", s2 != NULL);
    ASSERT("large out-of-range returns same placeholder as LEX__COUNT",
           strcmp(s, s2) == 0);

    PASS("out_of_range_stable");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_all_keys_non_empty();
    failures += test_identity_string();
    failures += test_voice_prefix();
    failures += test_footer_name();
    failures += test_footer_online();
    failures += test_wordmark_non_empty();
    failures += test_out_of_range_stable();

    if (failures == 0) {
        printf("All lexicon tests passed.\n");
        return 0;
    }
    printf("%d lexicon test(s) failed.\n", failures);
    return 1;
}
