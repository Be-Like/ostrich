#ifndef LEXICON_H
#define LEXICON_H

typedef enum {
    LEX_IDENTITY,       /* "OSTRICH // infiltration console" */
    LEX_WORDMARK,       /* the static ASCII ostrich banner   */
    LEX_FOOTER_NAME,    /* "ostrich"                          */
    LEX_FOOTER_ONLINE,  /* "ONLINE"                           */
    LEX_VOICE_PREFIX,   /* ">" (magenta voice signature)      */
    LEX__COUNT
} LexKey;

const char *lex(LexKey key);

#endif /* LEXICON_H */
