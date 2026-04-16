/*-------------------------------------------------------------------------
 *
 * pglitec.c
 *	  PGlite libc overrides
 *
 *
 *
 * NOTES
 *    this file contains "libc" function wrappers for PGlite, as well as 
 *    other flags used by PostgreSQL and needed by PGlite. These are 
 *    needed in order to emulate some system calls related to sockets, 
 *    user management etc.
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <setjmp.h>
#include <signal.h>
#include <string.h>

#include "pglitec.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
// TODO: an include for libpglite
#endif

volatile int is_pglite_active = 0;
volatile int pglite_single_user_startup_mode = 0;
volatile int pglite_direct_top_level_longjmp = 0;

#define MAX_EXIT_TRAPS 16

static sigjmp_buf pglite_exit_sigjmp_buf_stack[MAX_EXIT_TRAPS];
static volatile sig_atomic_t pglite_exit_status_stack[MAX_EXIT_TRAPS];
static volatile sig_atomic_t pglite_exit_trap_depth = 0;

void EMSCRIPTEN_KEEPALIVE clear_setitimer(void) {
    struct itimerval zero = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &zero, NULL);
}

int pgl_setPGliteActive(int newValue) {
	int current = is_pglite_active;
	is_pglite_active = newValue;
    if (newValue == 0) {
        clear_setitimer();
	}
	return current;
}

int pgl_set_single_user_startup_mode(int newValue) {
    int current = pglite_single_user_startup_mode;
    pglite_single_user_startup_mode = newValue;
    return current;
}

int pgl_set_direct_top_level_longjmp(int newValue) {
    int current = pglite_direct_top_level_longjmp;
    pglite_direct_top_level_longjmp = newValue;
    return current;
}

int EMSCRIPTEN_KEEPALIVE
pgl_push_exit_trap(sigjmp_buf **buf_out) {
    int trap_index;

    if (pglite_exit_trap_depth >= MAX_EXIT_TRAPS) {
        abort();
    }

    trap_index = pglite_exit_trap_depth++;
    pglite_exit_status_stack[trap_index] = 0;
    if (buf_out != NULL) {
        *buf_out = &pglite_exit_sigjmp_buf_stack[trap_index];
    }
    return trap_index;
}

int EMSCRIPTEN_KEEPALIVE
pgl_get_exit_trap_status_at(int trap_index) {
    if (trap_index < 0 || trap_index >= MAX_EXIT_TRAPS) {
        return 0;
    }
    return pglite_exit_status_stack[trap_index];
}

void EMSCRIPTEN_KEEPALIVE
pgl_pop_exit_trap(int trap_index) {
    if (trap_index < 0 || trap_index >= MAX_EXIT_TRAPS) {
        return;
    }
    if (pglite_exit_trap_depth <= trap_index) {
        return;
    }
    pglite_exit_trap_depth = trap_index;
    pglite_exit_status_stack[trap_index] = 0;
}

/* ========== Top level exception handling ==========
*
* In Postgres, the top level sigsetjmp handles exceptions encountered during executions
* In PGlite, we handle the top level sigsetjmp manually by exiting on the corresponding longjmp 
* with a predefined exit code (POSTGRES_MAIN_LONGJMP). We only need to override the longjmp 
* because setjmp already behaves as expected.
* This keeps the code changes cleaner.
*/

#define POSTGRES_MAIN_LONGJMP 100

volatile sigjmp_buf	postgresmain_sigjmp_buf;

volatile bool ignore_till_sync = false;
volatile bool send_ready_for_query = false;

/*
* This wraps the libc longjmp() to enable us to intercept and handle the main longjmp manually
*/
void EMSCRIPTEN_KEEPALIVE pgl_longjmp(jmp_buf env, int val) {
    if (is_pglite_active && memcmp(env, (void*)postgresmain_sigjmp_buf, sizeof(jmp_buf)) == 0) {
        if (pglite_direct_top_level_longjmp) {
            pglite_exit_trap_depth = 0;
            longjmp(env, val);
        }
        // reset this as it is expected
        if (!ignore_till_sync)
		    send_ready_for_query = true;	/* initially, or after error */
        pgl_exit(POSTGRES_MAIN_LONGJMP);
    }
    longjmp(env, val);
}

// emscripten defines siglongjmp as longjmp
void EMSCRIPTEN_KEEPALIVE pgl_siglongjmp(sigjmp_buf env, int val) {
    pgl_longjmp(env, val);
}

/* ========== Process handling functions ==========
*
* We wrap some process handling functions to emulate 
* the behavior of an OS when instantiating a new process.
* This is not available in emscripten atm, so we handle it manually
* See pglite.ts in the frontend on how we emulate this instantiation.
*/
static pgl_system_t pglite_system = NULL;

void EMSCRIPTEN_KEEPALIVE
pgl_set_system_fn(pgl_system_t system_fn) {
    pglite_system = system_fn;
}

int EMSCRIPTEN_KEEPALIVE
pgl_system(const char *command) {
    if (pglite_system) {
        return pglite_system(command);
    }
    // if pglite_system is not set, we assume we cannot exec that command and return != 0
    // we could also just call system() and let it crash, but this leads to some stderr messages on Windows
    return 123;
}

static pgl_popen_t pglite_popen = NULL;

void EMSCRIPTEN_KEEPALIVE
pgl_set_popen_fn(pgl_popen_t popen_fn) {
    pglite_popen = popen_fn;
}

FILE* EMSCRIPTEN_KEEPALIVE
pgl_popen(const char *command, const char *mode) {
    if (pglite_popen) {
        return pglite_popen(command, mode);
    }
    return popen(command, mode);
}

static pgl_pclose_t pglite_pclose = NULL;

void EMSCRIPTEN_KEEPALIVE
pgl_set_pclose_fn(pgl_pclose_t pclose_fn) {
    pglite_pclose = pclose_fn;
}

int EMSCRIPTEN_KEEPALIVE
pgl_pclose(FILE* stream) {
    if (pglite_pclose) {
        return pglite_pclose(stream);
    }
    return pclose(stream);
}

/* ========== User related functions ==========
*
* PostgreSQL code expects the current user to fulfill certain criteria to be allowed to run the process, otherwise it exits. 
* This is irrelevant in WASM/emscripten, so we fake the expected data to match what Postgres wants.
*/

#define PGLITE_UID 123

uid_t EMSCRIPTEN_KEEPALIVE
pgl_geteuid(void) {
    return PGLITE_UID;
}

uid_t EMSCRIPTEN_KEEPALIVE
pgl_getuid(void) {
    return PGLITE_UID;
}

struct passwd* EMSCRIPTEN_KEEPALIVE
pgl_getpwuid(uid_t uid) {
    static struct passwd pw;
    static char name[] = "postgres";
    static char passwd[] = "x";
    static char gecos[] = "Static User";
    static char dir[] = "/home/postgres";
    static char shell[] = "/bin/sh";

    pw.pw_name   = name;
    pw.pw_passwd = passwd;
    pw.pw_uid    = uid;
    pw.pw_gid    = uid;
    pw.pw_gecos  = gecos;
    pw.pw_dir    = dir;
    pw.pw_shell  = shell;

    return &pw;
}

/* ========== atexit functions ==========
*
* atexit registered functions provide important functionality in Postgres
* we need to be able to run them when closing PGlite.
*/
#define MAX_ATEXIT_FUNCS 32

static void (*atexit_funcs[MAX_ATEXIT_FUNCS])(void);
static int atexit_func_count = 0;

int EMSCRIPTEN_KEEPALIVE pgl_atexit(void (*function)(void)) {
    if (atexit_func_count >= MAX_ATEXIT_FUNCS) {
        // According to the C standard, atexit returns nonzero on failure.
        return -1;
    }
    atexit_funcs[atexit_func_count++] = function;
    return 0;
}

void EMSCRIPTEN_KEEPALIVE pgl_run_atexit_funcs(void) {
    // Call in reverse registration order
    for (int i = atexit_func_count - 1; i >= 0; --i) {
        if (atexit_funcs[i]) {
            atexit_funcs[i]();
        }
    }
    atexit_func_count = 0;
}

int EMSCRIPTEN_KEEPALIVE
pgl_atexit_checkpoint(void) {
    return atexit_func_count;
}

void EMSCRIPTEN_KEEPALIVE
pgl_run_atexit_from(int checkpoint) {
    if (checkpoint < 0) {
        checkpoint = 0;
    }
    if (checkpoint > atexit_func_count) {
        checkpoint = atexit_func_count;
    }

    for (int i = atexit_func_count - 1; i >= checkpoint; --i) {
        if (atexit_funcs[i]) {
            atexit_funcs[i]();
        }
    }
    atexit_func_count = checkpoint;
}

void EMSCRIPTEN_KEEPALIVE
pgl_discard_atexit_from(int checkpoint) {
    if (checkpoint < 0) {
        checkpoint = 0;
    }
    if (checkpoint > atexit_func_count) {
        checkpoint = atexit_func_count;
    }

    for (int i = checkpoint; i < atexit_func_count; ++i) {
        atexit_funcs[i] = NULL;
    }
    atexit_func_count = checkpoint;
}

/* ========== streams functions ==========
*
* initdb communicates with postgres via stdin<->stdout redirection
* we need to handle this manually mainly because we're also handling processes manually
*/

FILE* pgl_stdin = NULL;
FILE* pgl_stdout = NULL;

/*
* we override exit() to make sure we cleanup the stdin/stdout file descriptors
*/
void EMSCRIPTEN_KEEPALIVE
pgl_exit(int status) {
    if (pgl_stdin != NULL) {
        fclose(pgl_stdin);
        pgl_stdin = NULL;
    }
    if (pgl_stdout != NULL) {
        fflush(pgl_stdout);
        fclose(pgl_stdout);
        pgl_stdout = NULL;
    }
    optind = 1;
    if (pglite_exit_trap_depth > 0) {
        pglite_exit_status_stack[pglite_exit_trap_depth - 1] = status;
        siglongjmp(pglite_exit_sigjmp_buf_stack[pglite_exit_trap_depth - 1], 1);
    }
    exit(status);
}

/*
* Overrides freopen() libc function to allow initdb<->PGlite comm via standard streams (see initdb.ts in frontend)
*/
FILE * EMSCRIPTEN_KEEPALIVE
pgl_freopen(const char *pathname, const char *mode, int streamid) {
    if (streamid == 0) {
        pgl_stdin = freopen(pathname, mode, stdin);
        return pgl_stdin;
    }
    if (streamid == 1) {
        pgl_stdout = freopen(pathname, mode, stdout);
        return pgl_stdout;
    }
    if (streamid == 2) {
        return freopen(pathname, mode, stderr);
    }
    return NULL;
}

// ============ SHM ===============

typedef struct ShmSegment {
    int shmid;
    key_t key;
    size_t size;
    void *addr;
    int shmflg;
    struct ShmSegment *next;
} ShmSegment;

static ShmSegment *shm_list = NULL;
static unsigned int next_shmid = 1;

// shmget replacement
int EMSCRIPTEN_KEEPALIVE
pgl_shmget(key_t key, size_t size, int shmflg) {
    ShmSegment *seg = shm_list;

    // Search for existing segment
    while (seg) {
        if (seg->key == key) return seg->shmid;
        seg = seg->next;
    }

    // If IPC_CREAT is set, create new segment
    if (shmflg & IPC_CREAT) {
        int pagesize = getpagesize();
        while (pagesize < size) pagesize += pagesize;
        void *mem = malloc(pagesize);
        if (!mem) {
            errno = ENOMEM;
            return -1;
        }

        ShmSegment *new_seg = malloc(sizeof(ShmSegment));
        if (!new_seg) {
            free(mem);
            errno = ENOMEM;
            return -1;
        }

        new_seg->shmid = next_shmid++;
        new_seg->key = key;
        new_seg->size = size;
        new_seg->addr = mem;
        new_seg->shmflg = shmflg;
        new_seg->next = shm_list;
        shm_list = new_seg;

        return new_seg->shmid;
    }

    errno = ENOENT;
    return -1;
}

// shmat replacement
void EMSCRIPTEN_KEEPALIVE
*pgl_shmat(int shmid, const void *shmaddr, int shmflg) {
    ShmSegment *seg = shm_list;

    while (seg) {
        if (seg->shmid == shmid) {
            return seg->addr;
        }
        seg = seg->next;
    }

    errno = EINVAL;
    return (void *)-1;
}

// shmdt replacement
int EMSCRIPTEN_KEEPALIVE
pgl_shmdt(const void *shmaddr) {
    ShmSegment *seg = shm_list;

    while (seg) {
        if (seg->addr == shmaddr) {
            return 0;
        }
        seg = seg->next;
    }

    errno = EINVAL;
    return -1;
}

// shmctl replacement
int EMSCRIPTEN_KEEPALIVE
pgl_shmctl(int shmid, int cmd, struct shmid_ds *buf) {
    ShmSegment *seg = shm_list;
    ShmSegment *prev = NULL;

    while (seg) {
        if (seg->shmid == shmid) {
            if (cmd == IPC_RMID) {
                free(seg->addr);
                if (prev) prev->next = seg->next;
                else shm_list = seg->next;
                free(seg);
                return 0;
            } else if (cmd == IPC_STAT && buf != NULL) {
                buf->shm_segsz = seg->size;
                buf->shm_nattch = 0;
                buf->shm_atime = buf->shm_dtime = buf->shm_ctime = time(NULL);
                return 0;
            } else if (cmd == IPC_SET && buf != NULL) {
                seg->size = buf->shm_segsz;
                return 0;
            } else {
                fprintf(stderr, "pglitec: shmctl: no such cmd %d\n", cmd);
                errno = EINVAL;
                return -1;
            }
        }
        prev = seg;
        seg = seg->next;
    }

    fprintf(stderr, "pglitec: shmctl: no such segment %d\n", shmid);
    errno = EINVAL;
    return -1;
}

/* ========== MMAP/MUNMAP ==========
 * Dummy munmap implementation for emscripten.
 * Emscripten's munmap can corrupt unrelated files in MEMFS,
 * so we just return success without doing anything.
 * Memory will be reclaimed when the WASM instance terminates.
 */
int EMSCRIPTEN_KEEPALIVE
pgl_munmap(void *addr, size_t length) {
    (void)addr;
    (void)length;
    // dummy
    return 0;
}

/* ============ SOCKET EMULATION =============
*
* To exchange data between the backend (Postgres) and frontend (JS part of PGlite),
* we emulate a socket by overriding the following libc functions.
*/

/* 
* read FROM JS
* Callback used for reading data from the frontend
*/
pgl_read_t pgl_read;

/* write TO JS
* Callback used for writing data to the frontend
*/
pgl_write_t pgl_write;

/*
* Set the above callbacks
*/
void EMSCRIPTEN_KEEPALIVE
pgl_set_rw_cbs(pgl_read_t read_cb, pgl_write_t write_cb) {
    pgl_read = read_cb;
    pgl_write = write_cb;
}

int EMSCRIPTEN_KEEPALIVE pgl_fcntl(int __fd, int __cmd, ...) {
	// dummy 
	return 0;
}

int EMSCRIPTEN_KEEPALIVE pgl_setsockopt(int __fd, int __level, int __optname,
	const void *__optval, socklen_t __optlen) {
	// dummy 
	return 0;
}

int EMSCRIPTEN_KEEPALIVE pgl_getsockopt(int __fd, int __level, int __optname,
	void *__restrict __optval,
	socklen_t *__restrict __optlen) {
	// dummy 
	return 0;
}

int EMSCRIPTEN_KEEPALIVE pgl_getsockname(int __fd, struct sockaddr * __addr,
	socklen_t *__restrict __len) {
	// dummy 
	return 0;
}

/*
* Overrides the recv() libc function
*/

ssize_t EMSCRIPTEN_KEEPALIVE pgl_recv(int __fd, void *__buf, size_t __n, int __flags) {
	ssize_t got = pgl_read(__buf, __n);
	return got;
}

/*
* Overrides the send() libc function
*/

ssize_t EMSCRIPTEN_KEEPALIVE pgl_send(int __fd, const void *__buf, size_t __n, int __flags) {
	ssize_t wrote = pgl_write(__buf, __n);
	return wrote;
}

int EMSCRIPTEN_KEEPALIVE pgl_connect(int socket, const struct sockaddr *address, socklen_t address_len) {
	// dummy
	return 0;
}

struct pollfd {
    int   fd;         /* file descriptor */
    short events;     /* requested events */
	short revents;    /* returned events */
};

int EMSCRIPTEN_KEEPALIVE pgl_poll(struct pollfd fds[], ssize_t nfds, int timeout) {
    // dummy
	return nfds;
}
