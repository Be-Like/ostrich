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
    bool    remember;        /* opt-in SSH passkey persistence */
    char    passkey[256];    /* empty unless remember && auth==password */
    bool    kc_remember;     /* opt-in keychain passkey persistence */
    char    kc_passkey[256]; /* empty unless kc_remember */
} Conn;

typedef struct {
    Conn *items;
    int   count;
    int   mru_index; /* pre-selected on launch; 0 after load */
} ConnList;

typedef struct {
    char name[64];
    char project[1024];
    char scheme[256];
    char config[128];
    char bundle_id[256];
} Preset;

typedef struct {
    Preset *items;
    int     count;
    int     active_index; /* last-active for this conn; -1 none */
} PresetList;

typedef struct {
    char udid[128];
    char name[256]; /* cached display name */
} RememberedTarget;

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

void        store_conn_key(const Conn *c, char *buf, size_t cap);
StoreStatus preset_load(Arena *a, const char *conn_key, PresetList *out);
StoreStatus preset_save(const char *conn_key, const PresetList *l);
StoreStatus target_load(const char *conn_key, RememberedTarget *out);
StoreStatus target_save(const char *conn_key, const RememberedTarget *t);
StoreStatus scanroot_load(const char *conn_key, char *root, size_t cap);
StoreStatus scanroot_save(const char *conn_key, const char *root);

#ifdef __cplusplus
}
#endif

#endif /* STORE_H */
