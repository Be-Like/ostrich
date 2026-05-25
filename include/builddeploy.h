#ifndef BUILDDEPLOY_H
#define BUILDDEPLOY_H

#include "discovery.h"
#include "lexicon.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BD_OK = 0,
    BD_ERR_XCODE_MISSING, /* exit 127                           */
    BD_ERR_BUILD,         /* xcodebuild non-zero → build fail   */
    BD_ERR_BOOT,          /* boot/bootstatus     → deploy fail  */
    BD_ERR_INSTALL,       /* install non-zero    → deploy fail  */
    BD_ERR_LAUNCH,        /* launch non-zero     → deploy fail  */
    BD_ERR_PARSE,         /* settings did not parse             */
    BD_ERR_OOM
} BdStatus;

/* Full command-construction and parse interface is added in libbuilddeploy (T2). */

#ifdef __cplusplus
}
#endif

#endif /* BUILDDEPLOY_H */
