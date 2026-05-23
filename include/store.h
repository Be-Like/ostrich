#ifndef STORE_H
#define STORE_H

#include "arena.h"
#include "ssh.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    label[64];
    char    host[256];
    int     port;
    char    user[128];
    SshAuth auth;
    bool    remember;     /* opt-in passkey persistence */
    char    passkey[256]; /* empty unless remember && auth==password */
} Conn;

typedef struct {
    Conn *items;
    int   count;
    int   mru_index; /* pre-selected on launch; 0 after load */
} ConnList;

typedef enum {
    STORE_OK = 0,
    STORE_ERR_IO,
    STORE_ERR_PARSE,
    STORE_ERR_PERMS,
    STORE_ERR_OOM
} StoreStatus;

StoreStatus store_load(Arena *a, ConnList *out);
StoreStatus store_save(const ConnList *list); /* atomic + 0600 */
StoreStatus store_path(char *buf, size_t cap);
const char *store_status_str(StoreStatus st);

#ifdef __cplusplus
}
#endif

#endif /* STORE_H */
