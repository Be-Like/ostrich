#include "app.h"
#include "ui.h"
#include "ssh.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void app_form_to_ssh_config(const ConnForm *form, SshConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->host, sizeof(cfg->host), "%s", form->host);
    int port = (form->port[0] != '\0') ? atoi(form->port) : 22;
    cfg->port = (port > 0 && port <= 65535) ? port : 22;
    snprintf(cfg->user, sizeof(cfg->user), "%s", form->user);
    cfg->auth = form->auth;
    snprintf(cfg->passkey, sizeof(cfg->passkey), "%s", form->passkey);
}
