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

    /* LEX_CONN_UPLINK          */ "OSTRICH // UPLINK",
    /* LEX_CONN_BREACH          */ "BREACH",
    /* LEX_CONN_ABORT           */ "\xe2\x96\xa0 ABORT",
    /* LEX_CONN_KNOWN_HOSTS     */ "KNOWN HOSTS",
    /* LEX_CONN_NO_KNOWN_HOSTS  */ "// NO KNOWN HOSTS",

    /* LEX_CONN_BREACHING       */ "BREACHING PERIMETER\xe2\x80\xa6",
    /* LEX_CONN_ACCESS_GRANTED  */ "ACCESS GRANTED",
    /* LEX_CONN_WELCOME         */ "\xe2\x80\xa6WELCOME, OPERATOR.",
    /* LEX_CONN_ACCESS_DENIED   */ "ACCESS DENIED",
    /* LEX_CONN_ONLINE          */ "* ONLINE",
    /* LEX_CONN_REACQUIRING     */ "REACQUIRING SIGNAL\xe2\x80\xa6",
    /* LEX_CONN_SEVERED         */ "LINK SEVERED",

    /* LEX_CONN_ERR_NO_ROUTE         */ "HOST UNREACHABLE // NO ROUTE",
    /* LEX_CONN_ERR_PORT_CLOSED      */ "PERIMETER SEALED // PORT CLOSED",
    /* LEX_CONN_ERR_TIMEOUT          */ "NO RESPONSE // TIMEOUT",
    /* LEX_CONN_ERR_HOSTKEY_MISMATCH */ "HOST KEY MISMATCH // POSSIBLE INTERCEPTION",
    /* LEX_CONN_ERR_NO_SHELL         */ "NO FOOTHOLD // SHELL DENIED",
    /* LEX_CONN_UNKNOWN_HOST         */ "UNKNOWN HOST //",

    /* LEX_CONN_FIELD_HOST       */ "HOST",
    /* LEX_CONN_FIELD_PORT       */ "PORT",
    /* LEX_CONN_FIELD_USER       */ "USER",
    /* LEX_CONN_FIELD_AUTH       */ "AUTH",
    /* LEX_CONN_AUTH_AGENT       */ "SSH-AGENT",
    /* LEX_CONN_AUTH_PASSKEY     */ "PASSKEY",
    /* LEX_CONN_REMEMBER_PASSKEY */ "REMEMBER PASSKEY",
};

const char *lex(LexKey key) {
    if ((unsigned)key >= LEX__COUNT) return "(?)";
    return s_table[key];
}
