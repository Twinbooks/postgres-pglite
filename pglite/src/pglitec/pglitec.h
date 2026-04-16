#ifndef PGLITEC_H
#define PGLITEC_H

#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef ssize_t (*pgl_read_t)(void *buffer, size_t max_length);
typedef ssize_t (*pgl_write_t)(void *buffer, size_t length);
typedef int (*pgl_system_t)(const char *command);
typedef FILE *(*pgl_popen_t)(const char *command, const char *mode);
typedef int (*pgl_pclose_t)(FILE *stream);

int pgl_setPGliteActive(int newValue);
int pgl_set_single_user_startup_mode(int newValue);
int pgl_set_direct_top_level_longjmp(int newValue);
void pgl_set_rw_cbs(pgl_read_t read_cb, pgl_write_t write_cb);
void pgl_set_system_fn(pgl_system_t system_fn);
int pgl_system(const char *command);
void pgl_set_popen_fn(pgl_popen_t popen_fn);
FILE *pgl_popen(const char *command, const char *mode);
void pgl_set_pclose_fn(pgl_pclose_t pclose_fn);
int pgl_pclose(FILE *stream);
int pgl_atexit(void (*function)(void));
void pgl_run_atexit_funcs(void);
int pgl_atexit_checkpoint(void);
void pgl_run_atexit_from(int checkpoint);
void pgl_discard_atexit_from(int checkpoint);
void pgl_exit(int status);
uid_t pgl_geteuid(void);
uid_t pgl_getuid(void);
struct passwd *pgl_getpwuid(uid_t uid);

int pgl_enter_exit_trap(void);
int pgl_get_exit_trap_status(void);
void pgl_leave_exit_trap(void);

#ifdef __cplusplus
}
#endif

#endif
