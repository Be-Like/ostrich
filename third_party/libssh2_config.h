/* Minimal hand-crafted libssh2 config for Linux and macOS (OpenSSL backend).
 * Replaces the autotools/CMake-generated file.  Included via
 * -DHAVE_CONFIG_H -Ithird_party so libssh2_setup.h picks it up first. */
#ifndef LIBSSH2_CONFIG_H
#define LIBSSH2_CONFIG_H

#define HAVE_UNISTD_H       1
#define HAVE_INTTYPES_H     1
#define HAVE_SYS_TIME_H     1
#define HAVE_SYS_SELECT_H   1
#define HAVE_SYS_SOCKET_H   1
#define HAVE_SYS_IOCTL_H    1
#define HAVE_SYS_UIO_H      1
#define HAVE_SYS_UN_H       1
#define HAVE_ARPA_INET_H    1
#define HAVE_NETINET_IN_H   1
#define HAVE_POLL           1
#define HAVE_SELECT         1
#define HAVE_SNPRINTF       1
#define HAVE_GETTIMEOFDAY   1
#define HAVE_STRTOLL        1
#define HAVE_SOCKET         1
#define HAVE_INET_ADDR      1

#endif /* LIBSSH2_CONFIG_H */
