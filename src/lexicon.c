#include "lexicon.h"

static const char *s_table[LEX__COUNT] = {
    /* LEX_IDENTITY    */ "OSTRICH // infiltration console",
    /* LEX_WORDMARK    */
        "   .--.\n"
        "  (o.o)\n"
        "   |=|\n"
        " OSTRICH",
    /* LEX_FOOTER_NAME   */ "ostrich",
    /* LEX_FOOTER_ONLINE */ "ONLINE",
    /* LEX_VOICE_PREFIX  */ ">",
};

const char *lex(LexKey key) {
    if ((unsigned)key >= LEX__COUNT) return "(?)";
    return s_table[key];
}
