#include "postgres.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libpq/libpq.h"
#include "miscadmin.h"
#include "port.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"

#include "../../src/include/common/fe_memutils.h"
#include "../../src/include/common/file_utils.h"
#include "../../src/include/common/restricted_token.h"
#include "../../src/port/pg_config_paths.h"
#include "../src/pglitec/pglitec.h"
#include "libpglite.h"

extern int main(int argc, char *argv[]);
extern int pglite_initdb_main(int argc, char *argv[]);
extern void pgl_startPGlite(void);
extern void pgl_pq_flush(void);
extern void pgl_sendConnData(void);
extern Port *pgl_getMyProcPort(void);
extern int pgl_set_single_user_startup_mode(int newValue);
extern int pgl_set_direct_top_level_longjmp(int newValue);
extern sigjmp_buf postgresmain_sigjmp_buf;

struct PGlite {
    const unsigned char *input_data;
    size_t input_len;
    size_t input_offset;

    unsigned char *output_data;
    size_t output_len;
    size_t output_cap;

    char *data_dir;
    char *error;
    int ready;
    int closed;
};

static PGlite *active_db = NULL;
static char *global_error = NULL;
int pglite_embedded_initdb_mode = 0;

typedef struct EmbeddedInitdbCommand {
    FILE *stream;
    char *command;
    struct EmbeddedInitdbCommand *next;
} EmbeddedInitdbCommand;

typedef struct EmbeddedInitdbContext {
    PGlite *db;
    EmbeddedInitdbCommand *commands;
} EmbeddedInitdbContext;

typedef enum PGliteBootstrapMode {
    PGLITE_BOOTSTRAP_AUTO = 0,
    PGLITE_BOOTSTRAP_EMBEDDED,
} PGliteBootstrapMode;

static EmbeddedInitdbContext *active_initdb_context = NULL;

static void initialize_installation_paths(void);
static char *read_stream_excerpt(FILE *stream);

char *
pg_strdup(const char *in) {
    char *copy;

    copy = strdup(in != NULL ? in : "");
    if (copy == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return copy;
}

char *
pgl_pnstrdup(const char *in, Size size) {
    char *copy;
    size_t len;

    if (in == NULL) {
        fprintf(stderr, "cannot duplicate null pointer\n");
        exit(1);
    }

    len = strnlen(in, size);
    copy = malloc(len + 1);
    if (copy == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }

    memcpy(copy, in, len);
    copy[len] = '\0';
    return copy;
}

size_t
pgl_pvsnprintf(char *buf, size_t len, const char *fmt, va_list args) {
    int printed;

    printed = vsnprintf(buf, len, fmt, args);
    if (printed < 0) {
        fprintf(stderr, "vsnprintf failed for format string \"%s\"\n", fmt);
        exit(1);
    }

    if ((size_t) printed < len) {
        return (size_t) printed;
    }

    if ((size_t) printed > ((size_t) 0x3fffffff) - 1) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }

    return (size_t) printed + 1;
}

char *
pgl_psprintf(const char *fmt, ...) {
    int saved_errno;
    size_t len;

    saved_errno = errno;
    len = 128;

    for (;;) {
        char *result;
        va_list args;
        size_t newlen;

        result = pg_malloc(len);
        errno = saved_errno;
        va_start(args, fmt);
        newlen = pgl_pvsnprintf(result, len, fmt, args);
        va_end(args);

        if (newlen < len) {
            return result;
        }

        pg_free(result);
        len = newlen;
    }
}

void *
pg_malloc_extended(size_t size, int flags) {
    void *ptr;
    size_t alloc_size;

    alloc_size = size > 0 ? size : 1;
    if ((flags & MCXT_ALLOC_ZERO) != 0) {
        ptr = calloc(1, alloc_size);
    } else {
        ptr = malloc(alloc_size);
    }

    if (ptr == NULL && (flags & MCXT_ALLOC_NO_OOM) == 0) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return ptr;
}

void *
pg_malloc(size_t size) {
    return pg_malloc_extended(size, 0);
}

void *
pg_malloc0(size_t size) {
    return pg_malloc_extended(size, MCXT_ALLOC_ZERO);
}

void *
pg_realloc(void *ptr, size_t size) {
    void *resized;
    size_t alloc_size;

    alloc_size = size > 0 ? size : 1;
    resized = realloc(ptr, alloc_size);
    if (resized == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return resized;
}

void
pg_free(void *ptr) {
    free(ptr);
}

void
sync_pgdata(const char *pg_data, int serverVersion, DataDirSyncMethod sync_method) {
    (void) pg_data;
    (void) serverVersion;
    (void) sync_method;
}

void
get_restricted_token(void) {
}

pqsigfunc
pqsignal_fe(int signo, pqsigfunc func) {
    return signal(signo, func);
}

static void
set_string(char **target, const char *value) {
    free(*target);
    *target = value != NULL ? strdup(value) : NULL;
}

static void
set_global_error(const char *fmt, ...) {
    char buffer[2048];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    set_string(&global_error, buffer);
}

static void
set_db_error(PGlite *db, const char *fmt, ...) {
    char buffer[2048];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    set_string(&db->error, buffer);
}

static int
wait_status_from_exit_code(int exit_code) {
    if (exit_code < 0) {
        return -1;
    }
    if (exit_code > 255) {
        exit_code = 255;
    }
    return exit_code << 8;
}

static int
parse_bootstrap_mode_value(const char *value, PGliteBootstrapMode *out_mode) {
    if (value == NULL || value[0] == '\0' || strcmp(value, "auto") == 0) {
        *out_mode = PGLITE_BOOTSTRAP_AUTO;
        return 0;
    }
    if (strcmp(value, "embedded") == 0) {
        *out_mode = PGLITE_BOOTSTRAP_EMBEDDED;
        return 0;
    }
    return -1;
}

static int
resolve_bootstrap_mode(
    const char *requested_mode,
    PGliteBootstrapMode *out_mode,
    const char **out_value
) {
    const char *value;

    value = requested_mode;
    if (value == NULL || value[0] == '\0') {
        value = getenv("PGLITE_BOOTSTRAP_MODE");
    }
    if (out_value != NULL) {
        *out_value = value;
    }
    return parse_bootstrap_mode_value(value, out_mode);
}

static int
append_shell_arg(char ***argv, int *argc, int *cap, const char *arg) {
    char **resized;
    char *copy;
    int new_cap;

    if (*argc + 1 >= *cap) {
        new_cap = *cap > 0 ? (*cap * 2) : 8;
        resized = realloc(*argv, (size_t) new_cap * sizeof(char *));
        if (resized == NULL) {
            return -1;
        }
        *argv = resized;
        *cap = new_cap;
    }

    copy = strdup(arg);
    if (copy == NULL) {
        return -1;
    }

    (*argv)[*argc] = copy;
    (*argc)++;
    (*argv)[*argc] = NULL;
    return 0;
}

static void
free_shell_argv(char **argv, int argc) {
    int i;

    if (argv == NULL) {
        return;
    }

    for (i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

static int
split_shell_command(const char *command, char ***out_argv, int *out_argc) {
    enum {
        SHELL_STATE_NORMAL,
        SHELL_STATE_SINGLE_QUOTE,
        SHELL_STATE_DOUBLE_QUOTE,
    } state;
    char **argv;
    char *token;
    size_t command_len;
    size_t token_len;
    size_t i;
    int argc;
    int cap;
    int rc;
    unsigned char ch;

    *out_argv = NULL;
    *out_argc = 0;

    if (command == NULL) {
        return -1;
    }

    command_len = strlen(command);
    token = malloc(command_len + 1);
    if (token == NULL) {
        return -1;
    }

    argv = NULL;
    argc = 0;
    cap = 0;
    token_len = 0;
    state = SHELL_STATE_NORMAL;
    rc = -1;

    for (i = 0;; i++) {
        ch = (unsigned char) command[i];

        if (state == SHELL_STATE_NORMAL) {
            if (ch == '\0' || isspace(ch)) {
                if (token_len > 0) {
                    token[token_len] = '\0';
                    if (append_shell_arg(&argv, &argc, &cap, token) != 0) {
                        goto done;
                    }
                    token_len = 0;
                }
                if (ch == '\0') {
                    break;
                }
                continue;
            }

            if (ch == '\'') {
                state = SHELL_STATE_SINGLE_QUOTE;
                continue;
            }
            if (ch == '"') {
                state = SHELL_STATE_DOUBLE_QUOTE;
                continue;
            }
            if (ch == '\\' && command[i + 1] != '\0') {
                i++;
                ch = (unsigned char) command[i];
            }

            token[token_len++] = (char) ch;
            continue;
        }

        if (state == SHELL_STATE_SINGLE_QUOTE) {
            if (ch == '\0') {
                goto done;
            }
            if (ch == '\'') {
                state = SHELL_STATE_NORMAL;
                continue;
            }

            token[token_len++] = (char) ch;
            continue;
        }

        if (ch == '\0') {
            goto done;
        }
        if (ch == '"') {
            state = SHELL_STATE_NORMAL;
            continue;
        }
        if (ch == '\\' &&
            command[i + 1] != '\0' &&
            (command[i + 1] == '"' ||
             command[i + 1] == '\\' ||
             command[i + 1] == '$' ||
             command[i + 1] == '`' ||
             command[i + 1] == '\n')) {
            i++;
            ch = (unsigned char) command[i];
        }

        token[token_len++] = (char) ch;
    }

    if (state != SHELL_STATE_NORMAL) {
        goto done;
    }

    rc = 0;
    *out_argv = argv;
    *out_argc = argc;
    argv = NULL;

done:
    free(token);
    if (argv != NULL) {
        free_shell_argv(argv, argc);
    }
    return rc;
}

static void
restore_standard_stream(int saved_fd, int stream_fd) {
    if (saved_fd < 0) {
        return;
    }

    (void) dup2(saved_fd, stream_fd);
    (void) close(saved_fd);
}

static int
begin_output_capture(FILE **capture_stream, int *saved_stdout, int *saved_stderr) {
    int capture_fd;

    *capture_stream = tmpfile();
    if (*capture_stream == NULL) {
        return -1;
    }

    capture_fd = fileno(*capture_stream);
    if (capture_fd < 0) {
        (void) fclose(*capture_stream);
        *capture_stream = NULL;
        return -1;
    }

    fflush(NULL);
    *saved_stdout = dup(STDOUT_FILENO);
    *saved_stderr = dup(STDERR_FILENO);
    if (*saved_stdout < 0 || *saved_stderr < 0) {
        restore_standard_stream(*saved_stdout, STDOUT_FILENO);
        *saved_stdout = -1;
        restore_standard_stream(*saved_stderr, STDERR_FILENO);
        *saved_stderr = -1;
        (void) fclose(*capture_stream);
        *capture_stream = NULL;
        return -1;
    }

    if (dup2(capture_fd, STDOUT_FILENO) < 0 ||
        dup2(capture_fd, STDERR_FILENO) < 0) {
        restore_standard_stream(*saved_stdout, STDOUT_FILENO);
        *saved_stdout = -1;
        restore_standard_stream(*saved_stderr, STDERR_FILENO);
        *saved_stderr = -1;
        (void) fclose(*capture_stream);
        *capture_stream = NULL;
        return -1;
    }

    return 0;
}

static char *
end_output_capture(FILE **capture_stream, int *saved_stdout, int *saved_stderr) {
    char *capture_text = NULL;

    fflush(NULL);
    restore_standard_stream(*saved_stdout, STDOUT_FILENO);
    *saved_stdout = -1;
    restore_standard_stream(*saved_stderr, STDERR_FILENO);
    *saved_stderr = -1;

    if (*capture_stream != NULL) {
        capture_text = read_stream_excerpt(*capture_stream);
        (void) fclose(*capture_stream);
        *capture_stream = NULL;
    }

    return capture_text;
}

static char *
read_stream_excerpt(FILE *stream) {
    char *buffer;
    size_t len;
    size_t nread;

    if (stream == NULL) {
        return NULL;
    }

    if (fflush(stream) != 0) {
        return NULL;
    }
    if (fseek(stream, 0, SEEK_SET) != 0) {
        return NULL;
    }

    buffer = malloc(4097);
    if (buffer == NULL) {
        return NULL;
    }

    len = 0;
    while (len < 4096) {
        nread = fread(buffer + len, 1, 4096 - len, stream);
        len += nread;
        if (nread == 0) {
            break;
        }
    }
    buffer[len] = '\0';

    while (len > 0 && isspace((unsigned char) buffer[len - 1])) {
        len--;
        buffer[len] = '\0';
    }

    if (len == 0) {
        free(buffer);
        return NULL;
    }

    return buffer;
}

static void
cleanup_embedded_initdb_context(EmbeddedInitdbContext *ctx) {
    EmbeddedInitdbCommand *command;
    EmbeddedInitdbCommand *next;

    if (ctx == NULL) {
        return;
    }

    command = ctx->commands;
    while (command != NULL) {
        next = command->next;
        if (command->stream != NULL) {
            (void) fclose(command->stream);
        }
        free(command->command);
        free(command);
        command = next;
    }
    ctx->commands = NULL;
}

static int
ensure_output_capacity(PGlite *db, size_t additional) {
    size_t required = db->output_len + additional;
    unsigned char *resized;
    size_t new_cap;

    if (required <= db->output_cap) {
        return 0;
    }

    new_cap = db->output_cap > 0 ? db->output_cap : 8192;
    while (new_cap < required) {
        if (new_cap > ((size_t)-1) / 2) {
            new_cap = required;
            break;
        }
        new_cap *= 2;
    }

    resized = realloc(db->output_data, new_cap);
    if (resized == NULL) {
        set_db_error(db, "failed to allocate %zu bytes for query output", new_cap);
        return -1;
    }

    db->output_data = resized;
    db->output_cap = new_cap;
    return 0;
}

static void
reset_io(PGlite *db, const unsigned char *input_data, size_t input_len) {
    db->input_data = input_data;
    db->input_len = input_len;
    db->input_offset = 0;
    db->output_len = 0;
}

static int
copy_output(PGlite *db, char **out_data, size_t *out_len) {
    char *copy;

    *out_data = NULL;
    *out_len = 0;

    if (db->output_len == 0) {
        return 0;
    }

    copy = malloc(db->output_len);
    if (copy == NULL) {
        set_db_error(db, "failed to allocate %zu bytes for query result", db->output_len);
        return -1;
    }

    memcpy(copy, db->output_data, db->output_len);
    *out_data = copy;
    *out_len = db->output_len;
    return 0;
}

static void
store_be32(unsigned char *buffer, uint32_t value) {
    buffer[0] = (unsigned char) ((value >> 24) & 0xFF);
    buffer[1] = (unsigned char) ((value >> 16) & 0xFF);
    buffer[2] = (unsigned char) ((value >> 8) & 0xFF);
    buffer[3] = (unsigned char) (value & 0xFF);
}

static ssize_t
embedded_read(void *buffer, size_t max_length) {
    size_t remaining;
    size_t length;

    if (active_db == NULL || active_db->input_offset >= active_db->input_len) {
        return 0;
    }

    remaining = active_db->input_len - active_db->input_offset;
    length = remaining < max_length ? remaining : max_length;
    memcpy(buffer, active_db->input_data + active_db->input_offset, length);
    active_db->input_offset += length;
    return (ssize_t) length;
}

static ssize_t
embedded_write(void *buffer, size_t length) {
    if (active_db == NULL) {
        return -1;
    }

    if (ensure_output_capacity(active_db, length) != 0) {
        return -1;
    }

    memcpy(active_db->output_data + active_db->output_len, buffer, length);
    active_db->output_len += length;
    return (ssize_t) length;
}

static int
file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int
path_is_directory(const char *path) {
    struct stat st;

    if (stat(path, &st) != 0) {
        return 0;
    }

    return S_ISDIR(st.st_mode);
}

static void
build_runtime_dir(char *output, size_t output_size, const char *subdir, const char *fallback) {
    Dl_info info;
    char library_path[MAXPGPATH];
    char *slash;

    if (dladdr((void *) pglite_open, &info) == 0 || info.dli_fname == NULL) {
        strlcpy(output, fallback, output_size);
        return;
    }

    if (realpath(info.dli_fname, library_path) == NULL) {
        strlcpy(library_path, info.dli_fname, sizeof(library_path));
    }

    slash = strrchr(library_path, '/');
    if (slash == NULL) {
        strlcpy(output, fallback, output_size);
        return;
    }
    *slash = '\0';

    slash = strrchr(library_path, '/');
    if (slash == NULL) {
        strlcpy(output, fallback, output_size);
        return;
    }
    *slash = '\0';

    snprintf(output, output_size, "%s/%s", library_path, subdir);
}

static void
build_runtime_path(
    char *output,
    size_t output_size,
    const char *subdir,
    const char *filename,
    const char *fallback
) {
    char directory[MAXPGPATH];

    build_runtime_dir(directory, sizeof(directory), subdir, fallback);
    snprintf(output, output_size, "%s/%s", directory, filename);
}

static int
join_path(char *output, size_t output_size, const char *dir, const char *entry) {
    if (snprintf(output, output_size, "%s/%s", dir, entry) >= output_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int
directory_is_empty(const char *path) {
    DIR *dir;
    struct dirent *entry;
    int is_empty = 1;
    int saved_errno = 0;

    dir = opendir(path);
    if (dir == NULL) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        is_empty = 0;
        break;
    }

    if (closedir(dir) != 0) {
        saved_errno = errno;
        errno = saved_errno;
        return -1;
    }

    return is_empty;
}

static int
copy_file_contents(int source_fd, int target_fd) {
    char buffer[32768];

    for (;;) {
        ssize_t read_size = read(source_fd, buffer, sizeof(buffer));

        if (read_size == 0) {
            return 0;
        }

        if (read_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        for (ssize_t offset = 0; offset < read_size;) {
            ssize_t write_size = write(target_fd, buffer + offset, (size_t) (read_size - offset));

            if (write_size < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }

            offset += write_size;
        }
    }
}

static int
copy_regular_file(const char *source_path, const char *target_path, mode_t mode) {
    int source_fd;
    int target_fd;
    int rc = -1;
    int saved_errno = 0;

    source_fd = open(source_path, O_RDONLY);
    if (source_fd < 0) {
        return -1;
    }

    target_fd = open(target_path, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (target_fd < 0) {
        saved_errno = errno;
        close(source_fd);
        errno = saved_errno;
        return -1;
    }

    if (copy_file_contents(source_fd, target_fd) != 0) {
        saved_errno = errno;
        goto done;
    }

    if (fchmod(target_fd, mode & 0777) != 0) {
        saved_errno = errno;
        goto done;
    }

    rc = 0;

done:
    if (close(target_fd) != 0 && rc == 0) {
        saved_errno = errno;
        rc = -1;
    }
    if (close(source_fd) != 0 && rc == 0) {
        saved_errno = errno;
        rc = -1;
    }
    if (rc != 0) {
        errno = saved_errno;
    }
    return rc;
}

static int
copy_symbolic_link(const char *source_path, const char *target_path) {
    char link_target[MAXPGPATH];
    ssize_t link_size;

    link_size = readlink(source_path, link_target, sizeof(link_target) - 1);
    if (link_size < 0) {
        return -1;
    }

    link_target[link_size] = '\0';
    if (symlink(link_target, target_path) != 0) {
        return -1;
    }

    return 0;
}

static int
copy_directory_contents(const char *source_dir, const char *target_dir) {
    DIR *dir;
    struct dirent *entry;
    int rc = -1;
    int saved_errno = 0;

    dir = opendir(source_dir);
    if (dir == NULL) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char source_path[MAXPGPATH];
        char target_path[MAXPGPATH];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (join_path(source_path, sizeof(source_path), source_dir, entry->d_name) != 0 ||
            join_path(target_path, sizeof(target_path), target_dir, entry->d_name) != 0) {
            saved_errno = errno;
            goto done;
        }

        if (lstat(source_path, &st) != 0) {
            saved_errno = errno;
            goto done;
        }

        if (S_ISDIR(st.st_mode)) {
            if (mkdir(target_path, st.st_mode & 0777) != 0 && errno != EEXIST) {
                saved_errno = errno;
                goto done;
            }
            if (copy_directory_contents(source_path, target_path) != 0) {
                saved_errno = errno;
                goto done;
            }
            if (chmod(target_path, st.st_mode & 0777) != 0) {
                saved_errno = errno;
                goto done;
            }
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            if (copy_regular_file(source_path, target_path, st.st_mode) != 0) {
                saved_errno = errno;
                goto done;
            }
            continue;
        }

        if (S_ISLNK(st.st_mode)) {
            if (copy_symbolic_link(source_path, target_path) != 0) {
                saved_errno = errno;
                goto done;
            }
            continue;
        }

        saved_errno = ENOTSUP;
        goto done;
    }

    rc = 0;

done:
    if (closedir(dir) != 0 && rc == 0) {
        saved_errno = errno;
        rc = -1;
    }
    if (rc != 0) {
        errno = saved_errno;
    }
    return rc;
}

static int
remove_path_recursive(const char *path, int remove_top_level) {
    char child_path[MAXPGPATH];
    DIR *dir;
    struct dirent *entry;
    struct stat st;
    int rc;
    int saved_errno;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        return unlink(path);
    }

    dir = opendir(path);
    if (dir == NULL) {
        return -1;
    }
    rc = 0;
    saved_errno = 0;

    while (rc == 0) {
        entry = readdir(dir);
        if (entry == NULL) {
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (join_path(child_path, sizeof(child_path), path, entry->d_name) != 0) {
            saved_errno = errno;
            rc = -1;
            break;
        }
        if (remove_path_recursive(child_path, 1) != 0) {
            saved_errno = errno;
            rc = -1;
            break;
        }
    }

    if (closedir(dir) != 0 && rc == 0) {
        saved_errno = errno;
        rc = -1;
    }
    if (rc != 0) {
        errno = saved_errno;
        return -1;
    }

    if (!remove_top_level) {
        return 0;
    }
    if (rmdir(path) != 0) {
        return -1;
    }
    return 0;
}

static int
run_embedded_backend_command(PGlite *db, const char *command, FILE *input_stream) {
    char **parsed_argv;
    char **exec_argv;
    FILE *capture_stream;
    char *capture_text;
    int parsed_argc;
    int exec_argc;
    int i;
    int rc;
    int trapped;
    int status;
    int checkpoint;
    int saved_stdin;
    int saved_stdout;
    int saved_stderr;
    int input_fd;
    int capture_fd;

    parsed_argv = NULL;
    exec_argv = NULL;
    capture_stream = NULL;
    capture_text = NULL;
    parsed_argc = 0;
    exec_argc = 0;
    rc = -1;
    trapped = 0;
    status = 0;
    checkpoint = 0;
    saved_stdin = -1;
    saved_stdout = -1;
    saved_stderr = -1;
    input_fd = -1;
    capture_fd = -1;

    if (split_shell_command(command, &parsed_argv, &parsed_argc) != 0) {
        set_db_error(db, "failed to parse embedded initdb command '%s'", command);
        return -1;
    }

    exec_argv = calloc((size_t) parsed_argc + 1, sizeof(char *));
    if (exec_argv == NULL) {
        set_db_error(db, "failed to allocate embedded initdb command argv");
        goto done;
    }

    for (i = 0; i < parsed_argc; i++) {
        char *token = parsed_argv[i];

        if (token == NULL) {
            continue;
        }
        if (strcmp(token, ">") == 0 ||
            strcmp(token, "<") == 0 ||
            strcmp(token, "1>") == 0 ||
            strcmp(token, "2>") == 0 ||
            strcmp(token, ">>") == 0 ||
            strcmp(token, "1>>") == 0 ||
            strcmp(token, "2>>") == 0) {
            i++;
            continue;
        }
        if (token[0] == '>' ||
            token[0] == '<' ||
            (isdigit((unsigned char) token[0]) && (token[1] == '>' || token[1] == '<'))) {
            continue;
        }

        exec_argv[exec_argc++] = token;
        parsed_argv[i] = NULL;
    }
    exec_argv[exec_argc] = NULL;

    if (exec_argc == 0) {
        set_db_error(db, "embedded initdb command '%s' did not contain an executable", command);
        goto done;
    }

    if (fflush(input_stream) != 0 || fseek(input_stream, 0, SEEK_SET) != 0) {
        set_db_error(db, "failed to prepare embedded initdb input stream: %s", strerror(errno));
        goto done;
    }

    capture_stream = tmpfile();
    if (capture_stream == NULL) {
        set_db_error(db, "failed to create embedded initdb capture stream: %s", strerror(errno));
        goto done;
    }

    input_fd = fileno(input_stream);
    capture_fd = fileno(capture_stream);
    if (input_fd < 0 || capture_fd < 0) {
        set_db_error(db, "failed to access embedded initdb stream file descriptors: %s", strerror(errno));
        goto done;
    }

    fflush(NULL);
    saved_stdin = dup(STDIN_FILENO);
    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    if (saved_stdin < 0 || saved_stdout < 0 || saved_stderr < 0) {
        set_db_error(db, "failed to duplicate standard streams for embedded initdb: %s", strerror(errno));
        goto done;
    }

    if (dup2(input_fd, STDIN_FILENO) < 0 ||
        dup2(capture_fd, STDOUT_FILENO) < 0 ||
        dup2(capture_fd, STDERR_FILENO) < 0) {
        set_db_error(db, "failed to redirect standard streams for embedded initdb: %s", strerror(errno));
        goto done;
    }

    checkpoint = pgl_atexit_checkpoint();
    trapped = pgl_enter_exit_trap();
    if (!trapped) {
        status = main(exec_argc, exec_argv);
        pgl_leave_exit_trap();
    } else {
        status = pgl_get_exit_trap_status();
        pgl_leave_exit_trap();
    }

    pgl_run_atexit_from(checkpoint);
    fflush(NULL);

    restore_standard_stream(saved_stdin, STDIN_FILENO);
    saved_stdin = -1;
    restore_standard_stream(saved_stdout, STDOUT_FILENO);
    saved_stdout = -1;
    restore_standard_stream(saved_stderr, STDERR_FILENO);
    saved_stderr = -1;

    capture_text = read_stream_excerpt(capture_stream);
    if (status != 0) {
        if (capture_text != NULL) {
            set_db_error(
                db,
                "embedded initdb backend command failed with exit status %d: %s",
                status,
                capture_text
            );
        } else {
            set_db_error(
                db,
                "embedded initdb backend command '%s' failed with exit status %d",
                command,
                status
            );
        }
        rc = status;
        goto done;
    }

    rc = 0;

done:
    restore_standard_stream(saved_stdin, STDIN_FILENO);
    restore_standard_stream(saved_stdout, STDOUT_FILENO);
    restore_standard_stream(saved_stderr, STDERR_FILENO);
    if (capture_stream != NULL) {
        (void) fclose(capture_stream);
    }
    free(capture_text);
    if (exec_argv != NULL) {
        free_shell_argv(exec_argv, exec_argc);
    }
    if (parsed_argv != NULL) {
        free_shell_argv(parsed_argv, parsed_argc);
    }
    return rc;
}

static int
embedded_initdb_system(const char *command) {
    EmbeddedInitdbContext *ctx;

    ctx = active_initdb_context;
    if (ctx != NULL && ctx->db != NULL) {
        set_db_error(
            ctx->db,
            "embedded initdb requested unexpected external command '%s'",
            command != NULL ? command : "(null)"
        );
    }
    return wait_status_from_exit_code(127);
}

static FILE *
embedded_initdb_popen(const char *command, const char *mode) {
    EmbeddedInitdbContext *ctx;
    EmbeddedInitdbCommand *entry;
    FILE *stream;
    int saved_errno;

    ctx = active_initdb_context;
    if (ctx == NULL || ctx->db == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (mode == NULL || strcmp(mode, "w") != 0) {
        set_db_error(
            ctx->db,
            "embedded initdb does not support popen mode '%s' for '%s'",
            mode != NULL ? mode : "(null)",
            command != NULL ? command : "(null)"
        );
        errno = ENOTSUP;
        return NULL;
    }

    stream = tmpfile();
    if (stream == NULL) {
        set_db_error(ctx->db, "failed to create embedded initdb pipe for '%s': %s", command, strerror(errno));
        return NULL;
    }

    entry = calloc(1, sizeof(EmbeddedInitdbCommand));
    if (entry == NULL) {
        saved_errno = errno;
        (void) fclose(stream);
        errno = saved_errno;
        set_db_error(ctx->db, "failed to allocate embedded initdb command state");
        return NULL;
    }

    entry->command = strdup(command);
    if (entry->command == NULL) {
        saved_errno = errno;
        (void) fclose(stream);
        free(entry);
        errno = saved_errno;
        set_db_error(ctx->db, "failed to store embedded initdb command");
        return NULL;
    }

    entry->stream = stream;
    entry->next = ctx->commands;
    ctx->commands = entry;
    return stream;
}

static int
embedded_initdb_pclose(FILE *stream) {
    EmbeddedInitdbContext *ctx;
    EmbeddedInitdbCommand *entry;
    EmbeddedInitdbCommand **link;
    int exit_code;
    int wait_status;
    int saved_errno;

    if (stream == NULL) {
        errno = EINVAL;
        return -1;
    }

    ctx = active_initdb_context;
    if (ctx == NULL || ctx->db == NULL) {
        errno = EINVAL;
        return -1;
    }

    link = &ctx->commands;
    entry = ctx->commands;
    while (entry != NULL && entry->stream != stream) {
        link = &entry->next;
        entry = entry->next;
    }
    if (entry == NULL) {
        errno = EINVAL;
        return -1;
    }

    *link = entry->next;
    exit_code = run_embedded_backend_command(ctx->db, entry->command, entry->stream);
    saved_errno = errno;
    wait_status = wait_status_from_exit_code(exit_code);

    (void) fclose(entry->stream);
    free(entry->command);
    free(entry);

    errno = saved_errno;
    return wait_status;
}

static int
run_embedded_initdb(PGlite *db) {
    char pg_version_path[MAXPGPATH];
    char *argv[] = {
        my_exec_path,
        "-D",
        db->data_dir,
        "-A",
        "trust",
        "-U",
        "postgres",
        "--no-instructions",
        NULL,
    };
    int argc;
    int checkpoint;
    int trapped;
    int status;
    int saved_stdout;
    int saved_stderr;
    char *capture_text;
    FILE *capture_stream;
    EmbeddedInitdbContext ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.db = db;
    argc = (int) (sizeof(argv) / sizeof(argv[0])) - 1;
    saved_stdout = -1;
    saved_stderr = -1;
    capture_text = NULL;
    capture_stream = NULL;

    initialize_installation_paths();

    checkpoint = pgl_atexit_checkpoint();
    pglite_embedded_initdb_mode = 1;
    active_initdb_context = &ctx;
    pgl_set_system_fn(embedded_initdb_system);
    pgl_set_popen_fn(embedded_initdb_popen);
    pgl_set_pclose_fn(embedded_initdb_pclose);

    if (begin_output_capture(&capture_stream, &saved_stdout, &saved_stderr) != 0) {
        set_db_error(db, "failed to capture embedded initdb output: %s", strerror(errno));
        status = -1;
        goto done;
    }

    trapped = pgl_enter_exit_trap();
    if (!trapped) {
        status = pglite_initdb_main(argc, argv);
        pgl_leave_exit_trap();
    } else {
        status = pgl_get_exit_trap_status();
        pgl_leave_exit_trap();
    }
    capture_text = end_output_capture(&capture_stream, &saved_stdout, &saved_stderr);

done:
    restore_standard_stream(saved_stdout, STDOUT_FILENO);
    restore_standard_stream(saved_stderr, STDERR_FILENO);
    if (capture_stream != NULL) {
        (void) fclose(capture_stream);
    }
    pgl_discard_atexit_from(checkpoint);
    pgl_set_pclose_fn(NULL);
    pgl_set_popen_fn(NULL);
    pgl_set_system_fn(NULL);
    active_initdb_context = NULL;
    pglite_embedded_initdb_mode = 0;
    cleanup_embedded_initdb_context(&ctx);

    if (status != 0) {
        if (db->error == NULL || db->error[0] == '\0') {
            if (capture_text != NULL) {
                set_db_error(db, "embedded initdb exited with status %d: %s", status, capture_text);
            } else {
                set_db_error(db, "embedded initdb exited with status %d", status);
            }
        }
        free(capture_text);
        return -1;
    }

    snprintf(pg_version_path, sizeof(pg_version_path), "%s/PG_VERSION", db->data_dir);
    if (!file_exists(pg_version_path)) {
        set_db_error(
            db,
            "embedded initdb completed but '%s' is still missing",
            pg_version_path
        );
        free(capture_text);
        return -1;
    }

    free(capture_text);
    return 0;
}

static int
validate_bootstrap_target(PGlite *db, int *had_data_dir) {
    int empty_status;

    *had_data_dir = file_exists(db->data_dir);
    if (!*had_data_dir) {
        return 0;
    }

    if (!path_is_directory(db->data_dir)) {
        set_db_error(db, "data directory path '%s' is not a directory", db->data_dir);
        return -1;
    }

    empty_status = directory_is_empty(db->data_dir);
    if (empty_status < 0) {
        set_db_error(db, "failed to inspect data directory '%s': %s", db->data_dir, strerror(errno));
        return -1;
    }
    if (!empty_status) {
        set_db_error(
            db,
            "data directory '%s' exists but is not a PostgreSQL cluster and is not empty",
            db->data_dir
        );
        return -1;
    }

    return 0;
}

static int
bootstrap_data_dir(PGlite *db, PGliteBootstrapMode mode) {
    char pg_version_path[MAXPGPATH];
    int had_data_dir;

    (void) mode;
    had_data_dir = 0;

    snprintf(pg_version_path, sizeof(pg_version_path), "%s/PG_VERSION", db->data_dir);
    if (file_exists(pg_version_path)) {
        return 0;
    }

    if (validate_bootstrap_target(db, &had_data_dir) != 0) {
        return -1;
    }

    set_string(&db->error, NULL);
    if (run_embedded_initdb(db) == 0) {
        return 0;
    }

    (void) remove_path_recursive(db->data_dir, !had_data_dir);
    return -1;
}

static void
initialize_installation_paths(void) {
    if (my_exec_path[0] == '\0') {
        build_runtime_path(
            my_exec_path,
            MAXPGPATH,
            "bin",
            "postgres",
            PGBINDIR
        );
    }
    if (pkglib_path[0] == '\0') {
        build_runtime_dir(pkglib_path, MAXPGPATH, "lib", PKGLIBDIR);
    }
}

static int
run_single_user_main(PGlite *db) {
    char pg_version_path[MAXPGPATH];
    char *argv[] = {
        "postgres",
        "--single",
        "-F",
        "-O",
        "-j",
        "-c",
        "search_path=public",
        "-c",
        "exit_on_error=false",
        "-c",
        "log_checkpoints=false",
        "-c",
        "max_worker_processes=0",
        "-c",
        "max_parallel_workers=0",
        "-c",
        "max_parallel_workers_per_gather=0",
        "-D",
        db->data_dir,
        "postgres",
        NULL,
    };
    int argc = (int) (sizeof(argv) / sizeof(argv[0])) - 1;
    int saved_stdout;
    int saved_stderr;
    int trapped;
    int status;
    char *capture_text;
    FILE *capture_stream;

    saved_stdout = -1;
    saved_stderr = -1;
    capture_text = NULL;
    capture_stream = NULL;

    snprintf(pg_version_path, sizeof(pg_version_path), "%s/PG_VERSION", db->data_dir);
    if (!file_exists(pg_version_path)) {
        set_db_error(
            db,
            "data directory '%s' is not initialized; run initdb first",
            db->data_dir
        );
        return -1;
    }

    initialize_installation_paths();

    pgl_set_single_user_startup_mode(1);
    if (begin_output_capture(&capture_stream, &saved_stdout, &saved_stderr) != 0) {
        set_db_error(db, "failed to capture postgres startup output: %s", strerror(errno));
        pgl_set_single_user_startup_mode(0);
        return -1;
    }
    trapped = pgl_enter_exit_trap();
    if (!trapped) {
        (void) main(argc, argv);
        pgl_set_single_user_startup_mode(0);
        pgl_leave_exit_trap();
        free(end_output_capture(&capture_stream, &saved_stdout, &saved_stderr));
        return 0;
    }

    status = pgl_get_exit_trap_status();
    pgl_set_single_user_startup_mode(0);
    pgl_leave_exit_trap();
    capture_text = end_output_capture(&capture_stream, &saved_stdout, &saved_stderr);
    if (status != 0 && status != 99) {
        if (capture_text != NULL) {
            set_db_error(db, "unexpected postgres startup exit status %d: %s", status, capture_text);
        } else {
            set_db_error(db, "unexpected postgres startup exit status %d", status);
        }
        free(capture_text);
        return -1;
    }

    free(capture_text);
    return 0;
}

static int
process_startup_packet(PGlite *db, const char *message, size_t message_len) {
    int trapped;
    int status;
    int result;

    if (message_len < 8) {
        set_db_error(db, "startup packet is too short");
        return -1;
    }

    reset_io(db, (const unsigned char *) message, message_len);

    trapped = pgl_enter_exit_trap();
    if (!trapped) {
        result = ProcessStartupPacket(pgl_getMyProcPort(), true, true);
        pgl_leave_exit_trap();
    } else {
        status = pgl_get_exit_trap_status();
        pgl_leave_exit_trap();
        set_db_error(db, "startup packet processing exited with status %d", status);
        return -1;
    }

    if (result != 0) {
        set_db_error(db, "startup packet rejected with status %d", result);
        return -1;
    }

    pgl_sendConnData();
    pgl_pq_flush();
    return 0;
}

static int
process_query_message(PGlite *db, const char *message, size_t message_len) {
    int trapped;
    int status;

    reset_io(db, (const unsigned char *) message, message_len);

    while (db->input_offset < db->input_len || pq_buffer_remaining_data() > 0) {
        int old_direct_top_level_longjmp;

        PG_exception_stack = &postgresmain_sigjmp_buf;
        old_direct_top_level_longjmp = pgl_set_direct_top_level_longjmp(1);
        if (sigsetjmp(postgresmain_sigjmp_buf, 1) != 0) {
            pgl_set_direct_top_level_longjmp(old_direct_top_level_longjmp);
            PostgresMainLongJmp();
            PostgresMainResetAfterLongJmp();
            continue;
        }

        trapped = pgl_enter_exit_trap();
        if (!trapped) {
            PostgresMainLoopOnce();
            pgl_leave_exit_trap();
            pgl_set_direct_top_level_longjmp(old_direct_top_level_longjmp);
            continue;
        }

        status = pgl_get_exit_trap_status();
        pgl_leave_exit_trap();
        pgl_set_direct_top_level_longjmp(old_direct_top_level_longjmp);

        if (status == 100) {
            PostgresMainLongJmp();
            PostgresMainResetAfterLongJmp();
            continue;
        }

        set_db_error(db, "query processing exited with status %d", status);
        return -1;
    }

    PostgresSendReadyForQueryIfNecessary();
    pgl_pq_flush();
    return 0;
}

int
pglite_open_with_options(const char *data_dir, const char *bootstrap_mode, PGlite **out_db) {
    PGlite *db;
    PGliteBootstrapMode resolved_bootstrap_mode;
    const char *resolved_bootstrap_mode_value;
    int trapped;
    int status;

    *out_db = NULL;

    if (active_db != NULL) {
        set_global_error("only one embedded PGlite instance is supported per process");
        return -1;
    }

    if (data_dir == NULL || data_dir[0] == '\0') {
        set_global_error("data_dir is required");
        return -1;
    }

    resolved_bootstrap_mode_value = NULL;
    if (resolve_bootstrap_mode(
            bootstrap_mode,
            &resolved_bootstrap_mode,
            &resolved_bootstrap_mode_value
        ) != 0) {
        set_global_error(
            "unsupported bootstrap mode '%s'; expected auto or embedded",
            resolved_bootstrap_mode_value != NULL ? resolved_bootstrap_mode_value : ""
        );
        return -1;
    }

    db = calloc(1, sizeof(PGlite));
    if (db == NULL) {
        set_global_error("failed to allocate PGlite handle");
        return -1;
    }

    db->data_dir = strdup(data_dir);
    if (db->data_dir == NULL) {
        free(db);
        set_global_error("failed to allocate data_dir");
        return -1;
    }

    if (bootstrap_data_dir(db, resolved_bootstrap_mode) != 0) {
        set_global_error("%s", db->error != NULL ? db->error : "native bootstrap failed");
        free(db->data_dir);
        free(db->error);
        free(db);
        return -1;
    }

    active_db = db;
    pgl_set_rw_cbs(embedded_read, embedded_write);
    pgl_setPGliteActive(1);

    if (run_single_user_main(db) != 0) {
        set_global_error("%s", db->error != NULL ? db->error : "native startup failed");
        active_db = NULL;
        free(db->data_dir);
        free(db->error);
        free(db);
        return -1;
    }

    trapped = pgl_enter_exit_trap();
    if (!trapped) {
        pgl_startPGlite();
        pgl_leave_exit_trap();
    } else {
        status = pgl_get_exit_trap_status();
        pgl_leave_exit_trap();
        set_global_error("pgl_startPGlite exited with status %d", status);
        active_db = NULL;
        free(db->output_data);
        free(db->data_dir);
        free(db->error);
        free(db);
        return -1;
    }

    db->ready = 1;
    *out_db = db;
    set_string(&global_error, NULL);
    return 0;
}

int
pglite_open(const char *data_dir, PGlite **out_db) {
    return pglite_open_with_options(data_dir, NULL, out_db);
}

int
pglite_exec_protocol(
    PGlite *db,
    const char *message,
    size_t message_len,
    char **out_data,
    size_t *out_len
) {
    if (db == NULL || db->closed) {
        return -1;
    }

    if (message == NULL || message_len == 0) {
        set_db_error(db, "message is required");
        return -1;
    }

    set_string(&db->error, NULL);
    *out_data = NULL;
    *out_len = 0;

    if ((unsigned char) message[0] == 0) {
        if (process_startup_packet(db, message, message_len) != 0) {
            return -1;
        }
        return copy_output(db, out_data, out_len);
    }

    if (process_query_message(db, message, message_len) != 0) {
        return -1;
    }

    return copy_output(db, out_data, out_len);
}

int
pglite_exec(PGlite *db, const char *sql, char **out_data, size_t *out_len) {
    size_t sql_len;
    size_t message_len;
    unsigned char *message;
    int rc;

    if (db == NULL || db->closed) {
        return -1;
    }

    if (sql == NULL) {
        set_db_error(db, "sql is required");
        return -1;
    }

    sql_len = strlen(sql);
    message_len = sql_len + 6;
    message = malloc(message_len);
    if (message == NULL) {
        set_db_error(db, "failed to allocate %zu bytes for query message", message_len);
        return -1;
    }

    message[0] = 'Q';
    store_be32(message + 1, (uint32_t) (sql_len + 5));
    memcpy(message + 5, sql, sql_len);
    message[message_len - 1] = '\0';

    rc = pglite_exec_protocol(db, (const char *) message, message_len, out_data, out_len);
    free(message);
    return rc;
}

int
pglite_close(PGlite *db) {
    static const unsigned char terminate_message[] = { 'X', 0, 0, 0, 4 };
    int trapped;
    int status;

    if (db == NULL || db->closed) {
        return 0;
    }

    set_string(&db->error, NULL);
    pgl_setPGliteActive(0);
    reset_io(db, terminate_message, sizeof(terminate_message));

    trapped = pgl_enter_exit_trap();
    if (!trapped) {
        PostgresMainLoopOnce();
        pgl_leave_exit_trap();
    } else {
        status = pgl_get_exit_trap_status();
        pgl_leave_exit_trap();
        if (status != 0) {
            set_db_error(db, "unexpected close exit status %d", status);
            return -1;
        }
    }

    pgl_run_atexit_funcs();
    db->closed = 1;
    active_db = NULL;
    free(db->output_data);
    free(db->data_dir);
    free(db->error);
    free(db);
    return 0;
}

const char *
pglite_error(const PGlite *db) {
    if (db == NULL || db->error == NULL) {
        return "";
    }
    return db->error;
}

const char *
pglite_global_error(void) {
    return global_error != NULL ? global_error : "";
}

void
pglite_free(void *ptr) {
    free(ptr);
}
