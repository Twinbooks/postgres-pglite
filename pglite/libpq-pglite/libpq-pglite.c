#include "postgres_fe.h"

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>

#include "common/md5.h"
#include "libpq-fe.h"
#include "libpglite.h"

typedef struct PGLiteParam
{
    char                   *key;
    char                   *value;
    struct PGLiteParam     *next;
} PGLiteParam;

typedef struct PGLiteConnParams
{
    char   *raw_conninfo;
    char   *pglite_data_dir;
    char   *dbname;
    char   *user;
    char   *password;
    char   *host;
    char   *port;
    char   *options;
    char   *application_name;
    char   *sslmode;
    char   *connect_timeout;
    char   *target_session_attrs;
    char   *bootstrap_mode;
} PGLiteConnParams;

typedef struct PGLiteCell
{
    char   *value;
    int     len;
    bool    isnull;
} PGLiteCell;

typedef struct PGLiteListenChannel
{
    char                       *name;
    struct PGLiteListenChannel *next;
} PGLiteListenChannel;

typedef struct PGLiteEngine
{
    char                   *data_dir;
    char                   *bootstrap_mode;
    PGlite                 *db;
    int                     server_version;
    char                   *server_version_string;
    int                     refcount;
    struct pg_conn         *transaction_owner;
    char                    transaction_status;
    pthread_mutex_t         mutex;
    pthread_cond_t          cond;
    struct pg_conn         *connections;
    struct PGLiteEngine    *next;
} PGLiteEngine;

struct pg_result
{
    ExecStatusType          resultStatus;
    char                   *cmdStatus;
    char                   *cmdTuples;
    Oid                     oidValue;
    char                   *errorMessage;
    char                   *errorFields[256];
    int                     ntuples;
    int                     nfields;
    int                     binary;
    PGresAttDesc           *attDescs;
    PGLiteCell             *cells;
    struct pg_result       *next;
};

struct pg_cancel
{
    struct pg_conn         *conn;
};

struct pg_conn
{
    PGLiteEngine           *engine;
    ConnStatusType          status;
    PGTransactionStatusType xactStatus;
    char                   *errorMessage;
    char                   *dbname;
    char                   *user;
    char                   *password;
    char                   *host;
    char                   *port;
    char                   *options;
    char                   *application_name;
    char                   *data_dir;
    int                     backendPid;
    int                     serverVersion;
    bool                    usedPassword;
    bool                    nonblocking;
    PQnoticeProcessor       noticeProcessor;
    void                   *noticeProcessorArg;
    PGLiteParam            *parameters;
    PGresult               *resultHead;
    PGresult               *resultTail;
    PGnotify               *notifyHead;
    PGnotify               *notifyTail;
    PGLiteListenChannel    *listenChannels;
    int                     notifyReadFd;
    int                     notifyWriteFd;
    bool                    notifyPipeDirty;
    struct pg_conn         *engineNext;
};

static pthread_mutex_t engine_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static PGLiteEngine *engine_registry_head = NULL;
static pthread_once_t engine_registry_atexit_once = PTHREAD_ONCE_INIT;

static const char empty_string[] = "";

static bool probe_server_version(PGLiteEngine *engine, int *server_version, char **server_version_string);
static char *dup_string(const char *value);
static char *dup_bytes(const char *value, size_t len);

static void
free_notify_queue(PGnotify *notify)
{
    while (notify != NULL)
    {
        PGnotify *next = notify->next;

        free(notify);
        notify = next;
    }
}

static void
free_listen_channels(PGLiteListenChannel *channel)
{
    while (channel != NULL)
    {
        PGLiteListenChannel *next = channel->next;

        free(channel->name);
        free(channel);
        channel = next;
    }
}

static bool
set_nonblocking_fd(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool
init_notify_pipe(PGconn *conn)
{
    int pipe_fds[2];

    conn->notifyReadFd = -1;
    conn->notifyWriteFd = -1;

    if (pipe(pipe_fds) != 0)
        return false;
    if (!set_nonblocking_fd(pipe_fds[0]) || !set_nonblocking_fd(pipe_fds[1]))
    {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }

    conn->notifyReadFd = pipe_fds[0];
    conn->notifyWriteFd = pipe_fds[1];
    conn->notifyPipeDirty = false;
    return true;
}

static void
drain_notify_pipe(PGconn *conn)
{
    char buffer[64];

    if (conn == NULL || conn->notifyReadFd < 0)
        return;

    while (read(conn->notifyReadFd, buffer, sizeof(buffer)) > 0)
        ;
    conn->notifyPipeDirty = false;
}

static void
close_notify_pipe(PGconn *conn)
{
    if (conn == NULL)
        return;

    if (conn->notifyReadFd >= 0)
        close(conn->notifyReadFd);
    if (conn->notifyWriteFd >= 0)
        close(conn->notifyWriteFd);
    conn->notifyReadFd = -1;
    conn->notifyWriteFd = -1;
    conn->notifyPipeDirty = false;
}

static bool
conn_has_listener(PGconn *conn, const char *channel)
{
    PGLiteListenChannel *entry;

    if (conn == NULL || channel == NULL)
        return false;

    for (entry = conn->listenChannels; entry != NULL; entry = entry->next)
    {
        if (strcmp(entry->name, channel) == 0)
            return true;
    }
    return false;
}

static void
conn_listen_add(PGconn *conn, const char *channel)
{
    PGLiteListenChannel *entry;

    if (conn == NULL || channel == NULL || channel[0] == '\0' || conn_has_listener(conn, channel))
        return;

    entry = calloc(1, sizeof(*entry));
    if (entry == NULL)
        return;
    entry->name = dup_string(channel);
    if (entry->name == NULL)
    {
        free(entry);
        return;
    }
    entry->next = conn->listenChannels;
    conn->listenChannels = entry;
}

static void
conn_listen_remove(PGconn *conn, const char *channel)
{
    PGLiteListenChannel **entry_ptr;

    if (conn == NULL || channel == NULL)
        return;

    entry_ptr = &conn->listenChannels;
    while (*entry_ptr != NULL)
    {
        PGLiteListenChannel *entry = *entry_ptr;

        if (strcmp(entry->name, channel) == 0)
        {
            *entry_ptr = entry->next;
            free(entry->name);
            free(entry);
            return;
        }
        entry_ptr = &entry->next;
    }
}

static void
conn_listen_clear(PGconn *conn)
{
    if (conn == NULL)
        return;
    free_listen_channels(conn->listenChannels);
    conn->listenChannels = NULL;
}

static PGnotify *
alloc_notify_event(int be_pid, const char *channel, const char *extra)
{
    size_t channel_len;
    size_t extra_len;
    PGnotify *notify;
    char *storage;

    channel = channel != NULL ? channel : "";
    extra = extra != NULL ? extra : "";
    channel_len = strlen(channel);
    extra_len = strlen(extra);

    notify = calloc(1, sizeof(*notify) + channel_len + extra_len + 2);
    if (notify == NULL)
        return NULL;

    storage = (char *) notify + sizeof(*notify);
    notify->be_pid = be_pid;
    notify->relname = storage;
    memcpy(storage, channel, channel_len + 1);
    storage += channel_len + 1;
    notify->extra = storage;
    memcpy(storage, extra, extra_len + 1);
    return notify;
}

static void
queue_notify_event(PGconn *conn, PGnotify *notify)
{
    const char wake_byte = 'A';

    if (conn == NULL || notify == NULL)
    {
        free_notify_queue(notify);
        return;
    }

    notify->next = NULL;
    if (conn->notifyTail != NULL)
        conn->notifyTail->next = notify;
    else
        conn->notifyHead = notify;
    conn->notifyTail = notify;

    if (!conn->notifyPipeDirty && conn->notifyWriteFd >= 0)
    {
        if (write(conn->notifyWriteFd, &wake_byte, 1) >= 0)
            conn->notifyPipeDirty = true;
    }
}

static void
engine_register_conn(PGLiteEngine *engine, PGconn *conn)
{
    if (engine == NULL || conn == NULL)
        return;

    pthread_mutex_lock(&engine->mutex);
    conn->engineNext = engine->connections;
    engine->connections = conn;
    pthread_mutex_unlock(&engine->mutex);
}

static char *
dup_string(const char *value)
{
    size_t len;
    char *copy;

    if (value == NULL)
        return NULL;

    len = strlen(value) + 1;
    copy = malloc(len);
    if (copy != NULL)
        memcpy(copy, value, len);
    return copy;
}

static char *
dup_bytes(const char *value, size_t len)
{
    char *copy = malloc(len + 1);

    if (copy == NULL)
        return NULL;
    memcpy(copy, value, len);
    copy[len] = '\0';
    return copy;
}

static char *
fmt_string(const char *fmt, ...)
{
    va_list args;
    va_list copy_args;
    int len;
    char *buffer;

    va_start(args, fmt);
    va_copy(copy_args, args);
    len = vsnprintf(NULL, 0, fmt, copy_args);
    va_end(copy_args);
    if (len < 0)
    {
        va_end(args);
        return NULL;
    }

    buffer = malloc((size_t) len + 1);
    if (buffer == NULL)
    {
        va_end(args);
        return NULL;
    }

    vsnprintf(buffer, (size_t) len + 1, fmt, args);
    va_end(args);
    return buffer;
}

static void
set_conn_error(PGconn *conn, const char *fmt, ...)
{
    va_list args;
    va_list copy_args;
    int len;
    char *buffer;

    if (conn == NULL)
        return;

    free(conn->errorMessage);
    conn->errorMessage = NULL;

    va_start(args, fmt);
    va_copy(copy_args, args);
    len = vsnprintf(NULL, 0, fmt, copy_args);
    va_end(copy_args);
    if (len < 0)
    {
        va_end(args);
        return;
    }

    buffer = malloc((size_t) len + 1);
    if (buffer == NULL)
    {
        va_end(args);
        return;
    }

    vsnprintf(buffer, (size_t) len + 1, fmt, args);
    va_end(args);
    conn->errorMessage = buffer;
}

static const char *
nonnull_string(const char *value)
{
    return value != NULL ? value : empty_string;
}

static uint16_t
read_be16(const unsigned char *ptr)
{
    return (uint16_t) (((uint16_t) ptr[0] << 8) | ptr[1]);
}

static uint32_t
read_be32(const unsigned char *ptr)
{
    return ((uint32_t) ptr[0] << 24)
        | ((uint32_t) ptr[1] << 16)
        | ((uint32_t) ptr[2] << 8)
        | (uint32_t) ptr[3];
}

static int32_t
read_be32_signed(const unsigned char *ptr)
{
    return (int32_t) read_be32(ptr);
}

static int16_t
read_be16_signed(const unsigned char *ptr)
{
    return (int16_t) read_be16(ptr);
}

static void
free_parameters(PGLiteParam *param)
{
    while (param != NULL)
    {
        PGLiteParam *next = param->next;
        free(param->key);
        free(param->value);
        free(param);
        param = next;
    }
}

static void
free_conn_params(PGLiteConnParams *params)
{
    if (params == NULL)
        return;

    free(params->raw_conninfo);
    free(params->pglite_data_dir);
    free(params->dbname);
    free(params->user);
    free(params->password);
    free(params->host);
    free(params->port);
    free(params->options);
    free(params->application_name);
    free(params->sslmode);
    free(params->connect_timeout);
    free(params->target_session_attrs);
    free(params->bootstrap_mode);
}

static void
set_param_value(PGLiteParam **head, const char *key, const char *value)
{
    PGLiteParam *param;

    for (param = *head; param != NULL; param = param->next)
    {
        if (strcmp(param->key, key) == 0)
        {
            free(param->value);
            param->value = dup_string(value);
            return;
        }
    }

    param = calloc(1, sizeof(*param));
    if (param == NULL)
        return;
    param->key = dup_string(key);
    param->value = dup_string(value);
    param->next = *head;
    *head = param;
}

static const char *
find_param_value(PGLiteParam *head, const char *key)
{
    PGLiteParam *param;

    for (param = head; param != NULL; param = param->next)
    {
        if (strcmp(param->key, key) == 0)
            return param->value;
    }
    return NULL;
}

static bool
looks_like_path(const char *value)
{
    if (value == NULL || value[0] == '\0')
        return false;

    return value[0] == '/'
        || value[0] == '.'
        || value[0] == '~'
        || strchr(value, '/') != NULL;
}

static char *
read_conninfo_value(const char **cursor)
{
    const char *ptr = *cursor;
    char quote = '\0';
    char *buffer;
    size_t capacity = 32;
    size_t length = 0;

    if (*ptr == '\'' || *ptr == '"')
    {
        quote = *ptr;
        ptr++;
    }

    buffer = malloc(capacity);
    if (buffer == NULL)
        return NULL;

    while (*ptr != '\0')
    {
        char ch = *ptr;

        if (quote != '\0')
        {
            if (ch == quote)
            {
                ptr++;
                break;
            }
        }
        else if (isspace((unsigned char) ch))
        {
            break;
        }

        if (ch == '\\' && ptr[1] != '\0')
        {
            ptr++;
            ch = *ptr;
        }

        if (length + 2 > capacity)
        {
            char *grown;

            capacity *= 2;
            grown = realloc(buffer, capacity);
            if (grown == NULL)
            {
                free(buffer);
                return NULL;
            }
            buffer = grown;
        }

        buffer[length++] = ch;
        ptr++;
    }

    buffer[length] = '\0';
    while (isspace((unsigned char) *ptr))
        ptr++;
    *cursor = ptr;
    return buffer;
}

static void
assign_conninfo_value(PGLiteConnParams *params, const char *key, char *value)
{
    char **target = NULL;

    if (strcmp(key, "pglite_data_dir") == 0 || strcmp(key, "data_dir") == 0 || strcmp(key, "pglite_db") == 0)
        target = &params->pglite_data_dir;
    else if (strcmp(key, "dbname") == 0 || strcmp(key, "database") == 0)
        target = &params->dbname;
    else if (strcmp(key, "user") == 0)
        target = &params->user;
    else if (strcmp(key, "password") == 0)
        target = &params->password;
    else if (strcmp(key, "host") == 0)
        target = &params->host;
    else if (strcmp(key, "port") == 0)
        target = &params->port;
    else if (strcmp(key, "options") == 0)
        target = &params->options;
    else if (strcmp(key, "application_name") == 0)
        target = &params->application_name;
    else if (strcmp(key, "sslmode") == 0)
        target = &params->sslmode;
    else if (strcmp(key, "connect_timeout") == 0)
        target = &params->connect_timeout;
    else if (strcmp(key, "target_session_attrs") == 0)
        target = &params->target_session_attrs;
    else if (strcmp(key, "bootstrap_mode") == 0 || strcmp(key, "pglite_bootstrap_mode") == 0)
        target = &params->bootstrap_mode;

    if (target == NULL)
    {
        free(value);
        return;
    }

    free(*target);
    *target = value;
}

static bool
parse_conninfo_string(const char *conninfo, PGLiteConnParams *params, char **errmsg)
{
    const char *cursor;

    memset(params, 0, sizeof(*params));
    params->raw_conninfo = dup_string(nonnull_string(conninfo));

    if (conninfo == NULL)
        return true;

    while (isspace((unsigned char) *conninfo))
        conninfo++;

    if (*conninfo == '\0')
        return true;

    if (strchr(conninfo, '=') == NULL)
    {
        params->pglite_data_dir = dup_string(conninfo);
        return params->pglite_data_dir != NULL;
    }

    cursor = conninfo;
    while (*cursor != '\0')
    {
        const char *key_start;
        const char *key_end;
        char *key;
        char *value;

        while (isspace((unsigned char) *cursor))
            cursor++;
        if (*cursor == '\0')
            break;

        key_start = cursor;
        while (*cursor != '\0' && *cursor != '=' && !isspace((unsigned char) *cursor))
            cursor++;
        key_end = cursor;

        if (*cursor != '=')
        {
            if (errmsg != NULL)
                *errmsg = fmt_string("invalid conninfo token near \"%s\"", key_start);
            return false;
        }

        key = dup_bytes(key_start, (size_t) (key_end - key_start));
        if (key == NULL)
            return false;

        cursor++;
        value = read_conninfo_value(&cursor);
        if (value == NULL)
        {
            free(key);
            return false;
        }

        assign_conninfo_value(params, key, value);
        free(key);
    }

    if (params->pglite_data_dir == NULL
        && params->host == NULL
        && params->port == NULL
        && looks_like_path(params->dbname))
    {
        params->pglite_data_dir = params->dbname;
        params->dbname = dup_string("postgres");
        if (params->dbname == NULL)
            return false;
    }

    if (params->dbname == NULL)
        params->dbname = dup_string("postgres");
    if (params->user == NULL)
        params->user = dup_string("postgres");
    if (params->password == NULL)
        params->password = dup_string("");
    if (params->host == NULL)
        params->host = dup_string("");
    if (params->port == NULL)
        params->port = dup_string("");
    if (params->options == NULL)
        params->options = dup_string("");
    if (params->application_name == NULL)
        params->application_name = dup_string("");

    return params->dbname != NULL
        && params->user != NULL
        && params->password != NULL
        && params->host != NULL
        && params->port != NULL
        && params->options != NULL
        && params->application_name != NULL;
}

static PQconninfoOption *
build_conninfo_options(const PGLiteConnParams *params)
{
    static const struct
    {
        const char *keyword;
        const char *label;
        const char *dispchar;
        int         dispsize;
    } definitions[] = {
        {"pglite_data_dir", "PGlite Data Dir", "", 40},
        {"dbname", "Database", "", 20},
        {"user", "User", "", 20},
        {"password", "Password", "*", 20},
        {"host", "Host", "", 20},
        {"port", "Port", "", 8},
        {"options", "Options", "D", 20},
        {"application_name", "Application Name", "", 20},
        {"sslmode", "SSL Mode", "D", 12},
        {"connect_timeout", "Connect Timeout", "D", 8},
        {"target_session_attrs", "Target Session Attrs", "D", 20},
        {"bootstrap_mode", "Bootstrap Mode", "D", 12},
    };
    PQconninfoOption *options;
    size_t index;

    options = calloc(sizeof(definitions) / sizeof(definitions[0]) + 1, sizeof(*options));
    if (options == NULL)
        return NULL;

    for (index = 0; index < sizeof(definitions) / sizeof(definitions[0]); index++)
    {
        const char *value = NULL;

        options[index].keyword = (char *) definitions[index].keyword;
        options[index].label = (char *) definitions[index].label;
        options[index].dispchar = (char *) definitions[index].dispchar;
        options[index].dispsize = definitions[index].dispsize;

        if (strcmp(definitions[index].keyword, "pglite_data_dir") == 0)
            value = params->pglite_data_dir;
        else if (strcmp(definitions[index].keyword, "dbname") == 0)
            value = params->dbname;
        else if (strcmp(definitions[index].keyword, "user") == 0)
            value = params->user;
        else if (strcmp(definitions[index].keyword, "password") == 0)
            value = params->password;
        else if (strcmp(definitions[index].keyword, "host") == 0)
            value = params->host;
        else if (strcmp(definitions[index].keyword, "port") == 0)
            value = params->port;
        else if (strcmp(definitions[index].keyword, "options") == 0)
            value = params->options;
        else if (strcmp(definitions[index].keyword, "application_name") == 0)
            value = params->application_name;
        else if (strcmp(definitions[index].keyword, "sslmode") == 0)
            value = params->sslmode;
        else if (strcmp(definitions[index].keyword, "connect_timeout") == 0)
            value = params->connect_timeout;
        else if (strcmp(definitions[index].keyword, "target_session_attrs") == 0)
            value = params->target_session_attrs;
        else if (strcmp(definitions[index].keyword, "bootstrap_mode") == 0)
            value = params->bootstrap_mode;

        options[index].val = value != NULL ? dup_string(value) : NULL;
    }

    return options;
}

static void
free_result(PGresult *result)
{
    int index;

    if (result == NULL)
        return;

    free(result->cmdStatus);
    free(result->cmdTuples);
    free(result->errorMessage);

    for (index = 0; index < 256; index++)
        free(result->errorFields[index]);

    if (result->attDescs != NULL)
    {
        for (index = 0; index < result->nfields; index++)
            free(result->attDescs[index].name);
        free(result->attDescs);
    }

    if (result->cells != NULL)
    {
        int cell_count = result->ntuples * result->nfields;

        for (index = 0; index < cell_count; index++)
            free(result->cells[index].value);
        free(result->cells);
    }

    free(result);
}

static void
free_result_chain(PGresult *result)
{
    while (result != NULL)
    {
        PGresult *next = result->next;
        free_result(result);
        result = next;
    }
}

static PGresult *
alloc_result(ExecStatusType status)
{
    PGresult *result = calloc(1, sizeof(*result));

    if (result != NULL)
        result->resultStatus = status;
    return result;
}

static bool
append_result(PGresult **head, PGresult **tail, PGresult *result)
{
    if (result == NULL)
        return false;

    result->next = NULL;
    if (*tail != NULL)
        (*tail)->next = result;
    else
        *head = result;
    *tail = result;
    return true;
}

static bool
append_row_cell(PGresult *result, int row_index, int field_index, const char *value, int len, bool isnull)
{
    PGLiteCell *cell;
    int cell_index;

    if (result == NULL || result->nfields <= 0)
        return false;

    if (field_index == 0)
    {
        PGLiteCell *grown = realloc(result->cells, (size_t) (result->ntuples + 1) * (size_t) result->nfields * sizeof(*grown));

        if (grown == NULL)
            return false;
        result->cells = grown;
        memset(
            result->cells + ((size_t) result->ntuples * (size_t) result->nfields),
            0,
            (size_t) result->nfields * sizeof(*result->cells)
        );
        result->ntuples++;
    }

    cell_index = row_index * result->nfields + field_index;
    cell = &result->cells[cell_index];
    cell->isnull = isnull;
    cell->len = isnull ? 0 : len;
    cell->value = isnull ? NULL : dup_bytes(value, (size_t) len);
    return isnull || cell->value != NULL;
}

static char
parse_ready_status(const char *data, size_t len)
{
    size_t offset = 0;
    char status = '\0';

    while (offset + 5 <= len)
    {
        uint32_t message_len = read_be32((const unsigned char *) data + offset + 1);

        if (message_len < 4 || offset + 1 + message_len > len)
            break;

        if (data[offset] == 'Z' && message_len >= 5)
            status = data[offset + 5];

        offset += 1 + message_len;
    }

    return status;
}

static void
update_transaction_status(PGconn *conn, char status)
{
    if (conn == NULL)
        return;

    switch (status)
    {
        case 'I':
            conn->xactStatus = PQTRANS_IDLE;
            break;
        case 'T':
            conn->xactStatus = PQTRANS_INTRANS;
            break;
        case 'E':
            conn->xactStatus = PQTRANS_INERROR;
            break;
        default:
            conn->xactStatus = PQTRANS_UNKNOWN;
            break;
    }
}

static bool
engine_update_status(PGLiteEngine *engine, PGconn *conn, const char *data, size_t len)
{
    char status = parse_ready_status(data, len);

    if (status == '\0')
        return false;

    engine->transaction_status = status;
    if (status == 'T' || status == 'E')
        engine->transaction_owner = conn;
    else
        engine->transaction_owner = NULL;

    update_transaction_status(conn, status);
    return true;
}

static int
engine_exec(PGLiteEngine *engine, PGconn *conn, const char *query, char **out_data, size_t *out_len)
{
    int rc;

    pthread_mutex_lock(&engine->mutex);
    while (engine->transaction_owner != NULL && engine->transaction_owner != conn)
        pthread_cond_wait(&engine->cond, &engine->mutex);

    rc = pglite_exec(engine->db, query, out_data, out_len);
    if (rc == 0)
        engine_update_status(engine, conn, *out_data, *out_len);

    if (engine->transaction_owner == NULL)
        pthread_cond_broadcast(&engine->cond);

    pthread_mutex_unlock(&engine->mutex);
    return rc;
}

static void
engine_release(PGLiteEngine *engine)
{
    if (engine == NULL)
        return;

    pglite_close(engine->db);
    pthread_cond_destroy(&engine->cond);
    pthread_mutex_destroy(&engine->mutex);
    free(engine->data_dir);
    free(engine->bootstrap_mode);
    free(engine->server_version_string);
    free(engine);
}

static void
engine_registry_shutdown_at_exit(void)
{
    PGLiteEngine *engine;

    pthread_mutex_lock(&engine_registry_mutex);
    engine = engine_registry_head;
    engine_registry_head = NULL;
    pthread_mutex_unlock(&engine_registry_mutex);

    while (engine != NULL)
    {
        PGLiteEngine *next = engine->next;
        engine_release(engine);
        engine = next;
    }
}

static void
register_engine_registry_shutdown(void)
{
    atexit(engine_registry_shutdown_at_exit);
}

static PGLiteEngine *
engine_attach(const PGLiteConnParams *params, char **errmsg)
{
    PGLiteEngine *engine;
    int rc;

    pthread_once(&engine_registry_atexit_once, register_engine_registry_shutdown);
    pthread_mutex_lock(&engine_registry_mutex);
    for (engine = engine_registry_head; engine != NULL; engine = engine->next)
    {
        if (strcmp(engine->data_dir, nonnull_string(params->pglite_data_dir)) == 0)
        {
            engine->refcount++;
            pthread_mutex_unlock(&engine_registry_mutex);
            return engine;
        }
    }

    engine = calloc(1, sizeof(*engine));
    if (engine == NULL)
    {
        pthread_mutex_unlock(&engine_registry_mutex);
        return NULL;
    }

    engine->data_dir = dup_string(nonnull_string(params->pglite_data_dir));
    engine->bootstrap_mode = dup_string(params->bootstrap_mode);
    engine->server_version = PG_VERSION_NUM;
    engine->server_version_string = dup_string(PG_VERSION);
    engine->refcount = 1;
    engine->transaction_status = 'I';
    pthread_mutex_init(&engine->mutex, NULL);
    pthread_cond_init(&engine->cond, NULL);

    if (params->bootstrap_mode != NULL)
        rc = pglite_open_with_options(params->pglite_data_dir, params->bootstrap_mode, &engine->db);
    else
        rc = pglite_open(params->pglite_data_dir, &engine->db);

    if (rc != 0)
    {
        if (errmsg != NULL)
            *errmsg = dup_string(pglite_global_error());
        pthread_cond_destroy(&engine->cond);
        pthread_mutex_destroy(&engine->mutex);
        free(engine->data_dir);
        free(engine->bootstrap_mode);
        free(engine->server_version_string);
        free(engine);
        pthread_mutex_unlock(&engine_registry_mutex);
        return NULL;
    }

    (void) probe_server_version(engine, &engine->server_version, &engine->server_version_string);

    engine->next = engine_registry_head;
    engine_registry_head = engine;
    pthread_mutex_unlock(&engine_registry_mutex);
    return engine;
}

static void
engine_detach(PGLiteEngine *engine)
{
    if (engine == NULL)
        return;

    pthread_mutex_lock(&engine_registry_mutex);
    engine->refcount--;
    pthread_mutex_unlock(&engine_registry_mutex);
}

static bool
probe_server_version(PGLiteEngine *engine, int *server_version, char **server_version_string)
{
    char *out_data = NULL;
    size_t out_len = 0;
    size_t offset = 0;
    bool saw_row = false;

    if (pglite_exec(engine->db, "select current_setting('server_version_num'), current_setting('server_version')", &out_data, &out_len) != 0)
        return false;

    while (offset + 5 <= out_len)
    {
        uint32_t message_len = read_be32((const unsigned char *) out_data + offset + 1);
        const unsigned char *payload;

        if (message_len < 4 || offset + 1 + message_len > out_len)
            break;

        payload = (const unsigned char *) out_data + offset + 5;
        if (out_data[offset] == 'D')
        {
            uint16_t field_count = read_be16(payload);
            size_t payload_offset = 2;
            int field_index;

            for (field_index = 0; field_index < field_count; field_index++)
            {
                int32_t field_len = read_be32_signed(payload + payload_offset);

                payload_offset += 4;
                if (field_len < 0)
                    continue;

                if (field_index == 0)
                {
                    char *value = dup_bytes((const char *) payload + payload_offset, (size_t) field_len);

                    if (value != NULL)
                    {
                        *server_version = atoi(value);
                        free(value);
                    }
                }
                else if (field_index == 1)
                {
                    char *value = dup_bytes((const char *) payload + payload_offset, (size_t) field_len);

                    if (value != NULL)
                    {
                        free(*server_version_string);
                        *server_version_string = value;
                    }
                }

                payload_offset += (size_t) field_len;
            }
            saw_row = true;
        }

        offset += 1 + message_len;
    }

    pglite_free(out_data);
    return saw_row;
}

static void
parse_error_payload(PGresult *result, const unsigned char *payload, size_t payload_len)
{
    size_t offset = 0;

    while (offset < payload_len && payload[offset] != '\0')
    {
        unsigned char field_code = payload[offset++];
        const unsigned char *start = payload + offset;
        size_t field_len = 0;

        while (offset < payload_len && payload[offset] != '\0')
        {
            offset++;
            field_len++;
        }

        if (offset < payload_len)
        {
            free(result->errorFields[field_code]);
            result->errorFields[field_code] = dup_bytes((const char *) start, field_len);
            offset++;
        }
    }

    if (result->errorFields[(unsigned char) PG_DIAG_MESSAGE_PRIMARY] != NULL
        && result->errorFields[(unsigned char) PG_DIAG_SEVERITY_NONLOCALIZED] != NULL)
    {
        result->errorMessage = fmt_string(
            "%s: %s",
            result->errorFields[(unsigned char) PG_DIAG_SEVERITY_NONLOCALIZED],
            result->errorFields[(unsigned char) PG_DIAG_MESSAGE_PRIMARY]
        );
    }
    else if (result->errorFields[(unsigned char) PG_DIAG_MESSAGE_PRIMARY] != NULL)
    {
        result->errorMessage = dup_string(result->errorFields[(unsigned char) PG_DIAG_MESSAGE_PRIMARY]);
    }
    else
    {
        result->errorMessage = dup_string("unknown PostgreSQL error");
    }
}

static char *
extract_cmd_tuples(const char *command_tag)
{
    const char *ptr;

    if (command_tag == NULL)
        return dup_string("");

    ptr = command_tag + strlen(command_tag);
    while (ptr > command_tag && isdigit((unsigned char) ptr[-1]))
        ptr--;

    if (ptr == command_tag || !isdigit((unsigned char) *ptr))
        return dup_string("");

    return dup_string(ptr);
}

static Oid
extract_insert_oid(const char *command_tag)
{
    Oid oid = InvalidOid;

    if (command_tag != NULL && strncmp(command_tag, "INSERT ", 7) == 0)
    {
        const char *cursor = command_tag + 7;
        char *endptr = NULL;
        unsigned long value = strtoul(cursor, &endptr, 10);

        if (endptr != cursor)
            oid = (Oid) value;
    }
    return oid;
}

typedef enum PGLiteListenAction
{
    PGLITE_LISTEN_NONE = 0,
    PGLITE_LISTEN_ADD,
    PGLITE_LISTEN_REMOVE,
    PGLITE_LISTEN_REMOVE_ALL,
} PGLiteListenAction;

static const char *
skip_sql_space(const char *ptr)
{
    while (*ptr != '\0' && isspace((unsigned char) *ptr))
        ptr++;
    return ptr;
}

static char *
parse_sql_identifier_token(const char *ptr, const char **endptr)
{
    char *token = NULL;

    ptr = skip_sql_space(ptr);
    if (*ptr == '"')
    {
        const char *cursor = ++ptr;
        size_t length = 0;

        while (*cursor != '\0')
        {
            if (*cursor == '"' && cursor[1] != '"')
                break;
            if (*cursor == '"' && cursor[1] == '"')
                cursor++;
            cursor++;
            length++;
        }

        token = malloc(length + 1);
        if (token == NULL)
            return NULL;

        cursor = ptr;
        length = 0;
        while (*cursor != '\0')
        {
            if (*cursor == '"' && cursor[1] != '"')
                break;
            if (*cursor == '"' && cursor[1] == '"')
                cursor++;
            token[length++] = *cursor++;
        }
        token[length] = '\0';
        if (*cursor == '"')
            cursor++;
        *endptr = cursor;
        return token;
    }

    if (*ptr == '*')
    {
        token = dup_string("*");
        if (token != NULL)
            *endptr = ptr + 1;
        return token;
    }

    if (!(isalpha((unsigned char) *ptr) || *ptr == '_'))
    {
        *endptr = ptr;
        return NULL;
    }

    {
        const char *cursor = ptr;
        size_t length;

        while (isalnum((unsigned char) *cursor) || *cursor == '_')
            cursor++;
        length = (size_t) (cursor - ptr);
        token = dup_bytes(ptr, length);
        if (token == NULL)
            return NULL;
        *endptr = cursor;
        return token;
    }
}

static PGLiteListenAction
parse_listen_action(const char *query, char **channel)
{
    const char *ptr;
    char *token;

    *channel = NULL;
    if (query == NULL)
        return PGLITE_LISTEN_NONE;

    ptr = skip_sql_space(query);
    if (strncasecmp(ptr, "listen", 6) == 0 && !isalnum((unsigned char) ptr[6]) && ptr[6] != '_')
    {
        token = parse_sql_identifier_token(ptr + 6, &ptr);
        if (token == NULL)
            return PGLITE_LISTEN_NONE;
        *channel = token;
        return PGLITE_LISTEN_ADD;
    }

    if (strncasecmp(ptr, "unlisten", 8) == 0 && !isalnum((unsigned char) ptr[8]) && ptr[8] != '_')
    {
        token = parse_sql_identifier_token(ptr + 8, &ptr);
        if (token == NULL)
            return PGLITE_LISTEN_NONE;
        *channel = token;
        if (strcmp(token, "*") == 0)
        {
            free(token);
            *channel = NULL;
            return PGLITE_LISTEN_REMOVE_ALL;
        }
        return PGLITE_LISTEN_REMOVE;
    }

    return PGLITE_LISTEN_NONE;
}

static void
apply_listen_action(PGconn *conn, const char *query)
{
    PGLiteListenAction action;
    char *channel = NULL;

    action = parse_listen_action(query, &channel);
    if (action == PGLITE_LISTEN_NONE || conn == NULL || conn->engine == NULL)
    {
        free(channel);
        return;
    }

    pthread_mutex_lock(&conn->engine->mutex);
    switch (action)
    {
        case PGLITE_LISTEN_ADD:
            conn_listen_add(conn, channel);
            break;
        case PGLITE_LISTEN_REMOVE:
            conn_listen_remove(conn, channel);
            break;
        case PGLITE_LISTEN_REMOVE_ALL:
            conn_listen_clear(conn);
            break;
        default:
            break;
    }
    pthread_mutex_unlock(&conn->engine->mutex);
    free(channel);
}

static void
fanout_notification(PGconn *conn, int be_pid, const char *channel, const char *extra)
{
    PGconn *listener;

    if (conn == NULL || conn->engine == NULL || channel == NULL || channel[0] == '\0')
        return;

    pthread_mutex_lock(&conn->engine->mutex);
    for (listener = conn->engine->connections; listener != NULL; listener = listener->engineNext)
    {
        PGnotify *notify;

        if (!conn_has_listener(listener, channel))
            continue;

        notify = alloc_notify_event(be_pid, channel, extra);
        if (notify == NULL)
            continue;
        queue_notify_event(listener, notify);
    }
    pthread_mutex_unlock(&conn->engine->mutex);
}

static bool
results_succeeded(const PGresult *result)
{
    const PGresult *entry;

    for (entry = result; entry != NULL; entry = entry->next)
    {
        if (entry->resultStatus == PGRES_FATAL_ERROR || entry->resultStatus == PGRES_BAD_RESPONSE)
            return false;
    }
    return true;
}

static bool
parse_results(PGconn *conn, const char *data, size_t len, PGresult **head, PGresult **tail)
{
    size_t offset = 0;
    PGresult *current = NULL;

    *head = NULL;
    *tail = NULL;

    while (offset + 5 <= len)
    {
        unsigned char message_type = (unsigned char) data[offset];
        uint32_t message_len = read_be32((const unsigned char *) data + offset + 1);
        const unsigned char *payload;

        if (message_len < 4 || offset + 1 + message_len > len)
        {
            set_conn_error(conn, "malformed backend message stream");
            free_result(current);
            free_result_chain(*head);
            *head = NULL;
            *tail = NULL;
            return false;
        }

        payload = (const unsigned char *) data + offset + 5;
        switch (message_type)
        {
            case 'T':
            {
                uint16_t field_count = read_be16(payload);
                size_t payload_offset = 2;
                int field_index;

                free_result(current);
                current = alloc_result(PGRES_TUPLES_OK);
                if (current == NULL)
                    return false;

                current->nfields = field_count;
                current->attDescs = calloc(field_count, sizeof(*current->attDescs));
                if (current->attDescs == NULL)
                {
                    free_result(current);
                    return false;
                }

                for (field_index = 0; field_index < field_count; field_index++)
                {
                    const unsigned char *name_start = payload + payload_offset;
                    size_t name_len = 0;

                    while (payload_offset < message_len - 4 && payload[payload_offset] != '\0')
                    {
                        payload_offset++;
                        name_len++;
                    }
                    current->attDescs[field_index].name = dup_bytes((const char *) name_start, name_len);
                    payload_offset++;
                    current->attDescs[field_index].tableid = read_be32(payload + payload_offset);
                    payload_offset += 4;
                    current->attDescs[field_index].columnid = read_be16(payload + payload_offset);
                    payload_offset += 2;
                    current->attDescs[field_index].typid = read_be32(payload + payload_offset);
                    payload_offset += 4;
                    current->attDescs[field_index].typlen = read_be16_signed(payload + payload_offset);
                    payload_offset += 2;
                    current->attDescs[field_index].atttypmod = read_be32_signed(payload + payload_offset);
                    payload_offset += 4;
                    current->attDescs[field_index].format = read_be16(payload + payload_offset);
                    payload_offset += 2;
                }
                break;
            }

            case 'D':
            {
                uint16_t field_count = read_be16(payload);
                size_t payload_offset = 2;
                int row_index;
                int field_index;

                if (current == NULL)
                {
                    set_conn_error(conn, "received DataRow without RowDescription");
                    free_result_chain(*head);
                    return false;
                }

                row_index = current->ntuples;
                for (field_index = 0; field_index < field_count; field_index++)
                {
                    int32_t field_len = read_be32_signed(payload + payload_offset);

                    payload_offset += 4;
                    if (field_len < 0)
                    {
                        if (!append_row_cell(current, row_index, field_index, NULL, 0, true))
                        {
                            free_result(current);
                            free_result_chain(*head);
                            return false;
                        }
                        continue;
                    }

                    if (!append_row_cell(current, row_index, field_index, (const char *) payload + payload_offset, field_len, false))
                    {
                        free_result(current);
                        free_result_chain(*head);
                        return false;
                    }
                    payload_offset += (size_t) field_len;
                }
                break;
            }

            case 'C':
            {
                size_t tag_len = (size_t) message_len - 5;

                if (current == NULL)
                    current = alloc_result(PGRES_COMMAND_OK);
                if (current == NULL)
                    return false;

                current->cmdStatus = dup_bytes((const char *) payload, tag_len);
                current->cmdTuples = extract_cmd_tuples(current->cmdStatus);
                current->oidValue = extract_insert_oid(current->cmdStatus);
                if (current->nfields == 0)
                    current->resultStatus = PGRES_COMMAND_OK;

                if (!append_result(head, tail, current))
                {
                    free_result(current);
                    free_result_chain(*head);
                    return false;
                }
                current = NULL;
                break;
            }

            case 'I':
            {
                PGresult *result = alloc_result(PGRES_EMPTY_QUERY);

                if (result == NULL || !append_result(head, tail, result))
                {
                    free_result(result);
                    free_result_chain(*head);
                    return false;
                }
                break;
            }

            case 'E':
            {
                PGresult *result = alloc_result(PGRES_FATAL_ERROR);

                if (result == NULL)
                {
                    free_result_chain(*head);
                    return false;
                }
                parse_error_payload(result, payload, (size_t) message_len - 4);
                if (!append_result(head, tail, result))
                {
                    free_result(result);
                    free_result_chain(*head);
                    return false;
                }
                set_conn_error(conn, "%s", nonnull_string(result->errorMessage));
                free_result(current);
                current = NULL;
                break;
            }

            case 'N':
            {
                PGresult *notice = alloc_result(PGRES_NONFATAL_ERROR);

                if (notice == NULL)
                {
                    free_result(current);
                    free_result_chain(*head);
                    return false;
                }
                parse_error_payload(notice, payload, (size_t) message_len - 4);
                if (conn->noticeProcessor != NULL)
                    conn->noticeProcessor(conn->noticeProcessorArg, nonnull_string(notice->errorMessage));
                free_result(notice);
                break;
            }

            case 'S':
            {
                const char *key = (const char *) payload;
                size_t key_len = strlen(key);
                const char *value = key + key_len + 1;

                set_param_value(&conn->parameters, key, value);
                break;
            }

            case 'K':
                if (message_len >= 12)
                    conn->backendPid = (int) read_be32(payload);
                break;

            case 'A':
            {
                int be_pid = (int) read_be32(payload);
                const char *channel = (const char *) payload + 4;
                size_t channel_len = strlen(channel);
                const char *extra = channel + channel_len + 1;

                fanout_notification(conn, be_pid, channel, extra);
                break;
            }

            case 'Z':
                break;

            case '1':
            case '2':
            case '3':
            case 'n':
            case 's':
            case 't':
                break;

            default:
                break;
        }

        offset += 1 + message_len;
    }

    free_result(current);
    return true;
}

static bool
exec_query_to_results(PGconn *conn, const char *query, PGresult **head, PGresult **tail)
{
    char *out_data = NULL;
    size_t out_len = 0;
    int rc;

    if (conn == NULL || conn->engine == NULL)
        return false;

    free(conn->errorMessage);
    conn->errorMessage = dup_string("");

    rc = engine_exec(conn->engine, conn, query, &out_data, &out_len);
    if (rc != 0)
    {
        set_conn_error(conn, "%s", pglite_error(conn->engine->db));
        update_transaction_status(conn, conn->engine->transaction_status);
        return false;
    }

    if (!parse_results(conn, out_data, out_len, head, tail))
    {
        pglite_free(out_data);
        return false;
    }

    if (results_succeeded(*head))
        apply_listen_action(conn, query);

    if (parse_ready_status(out_data, out_len) != '\0')
        update_transaction_status(conn, conn->engine->transaction_status);
    pglite_free(out_data);
    return true;
}

static void
clear_pending_results(PGconn *conn)
{
    if (conn == NULL)
        return;
    free_result_chain(conn->resultHead);
    conn->resultHead = NULL;
    conn->resultTail = NULL;
}

PGconn *
PQconnectStart(const char *conninfo)
{
    return PQconnectdb(conninfo);
}

PostgresPollingStatusType
PQconnectPoll(PGconn *conn)
{
    return PQstatus(conn) == CONNECTION_OK ? PGRES_POLLING_OK : PGRES_POLLING_FAILED;
}

PGconn *
PQconnectdb(const char *conninfo)
{
    PGconn *conn = calloc(1, sizeof(*conn));
    PGLiteConnParams params;
    char *errmsg = NULL;

    if (conn == NULL)
        return NULL;

    conn->status = CONNECTION_BAD;
    conn->xactStatus = PQTRANS_IDLE;
    conn->backendPid = (int) getpid();
    conn->errorMessage = dup_string("");
    conn->noticeProcessor = NULL;
    conn->noticeProcessorArg = NULL;

    if (!parse_conninfo_string(conninfo, &params, &errmsg))
    {
        set_conn_error(conn, "%s", errmsg != NULL ? errmsg : "failed to parse conninfo");
        free(errmsg);
        return conn;
    }

    if (params.pglite_data_dir == NULL || params.pglite_data_dir[0] == '\0')
    {
        set_conn_error(conn, "embedded libpq-pglite requires pglite_data_dir");
        free_conn_params(&params);
        return conn;
    }

    conn->dbname = dup_string(params.dbname);
    conn->user = dup_string(params.user);
    conn->password = dup_string(params.password);
    conn->host = dup_string(params.host);
    conn->port = dup_string(params.port);
    conn->options = dup_string(params.options);
    conn->application_name = dup_string(params.application_name);
    conn->data_dir = dup_string(params.pglite_data_dir);
    conn->usedPassword = params.password != NULL && params.password[0] != '\0';

    conn->engine = engine_attach(&params, &errmsg);
    if (conn->engine == NULL)
    {
        set_conn_error(conn, "%s", errmsg != NULL ? errmsg : "failed to open embedded engine");
        free(errmsg);
        free_conn_params(&params);
        return conn;
    }

    if (!init_notify_pipe(conn))
    {
        set_conn_error(conn, "failed to initialize notification pipe");
        engine_detach(conn->engine);
        conn->engine = NULL;
        free_conn_params(&params);
        return conn;
    }
    engine_register_conn(conn->engine, conn);

    set_param_value(&conn->parameters, "client_encoding", "UTF8");
    set_param_value(&conn->parameters, "server_encoding", "UTF8");
    set_param_value(&conn->parameters, "standard_conforming_strings", "on");
    set_param_value(&conn->parameters, "integer_datetimes", "on");
    set_param_value(&conn->parameters, "session_authorization", nonnull_string(conn->user));
    set_param_value(&conn->parameters, "is_superuser", "on");
    if (conn->application_name != NULL && conn->application_name[0] != '\0')
        set_param_value(&conn->parameters, "application_name", conn->application_name);

    conn->serverVersion = conn->engine->server_version;
    if (conn->engine->server_version > 0)
    {
        char *server_version_num = fmt_string("%d", conn->engine->server_version);

        if (server_version_num != NULL)
        {
            set_param_value(&conn->parameters, "server_version_num", server_version_num);
            free(server_version_num);
        }
    }
    if (conn->engine->server_version_string != NULL)
        set_param_value(&conn->parameters, "server_version", conn->engine->server_version_string);
    conn->status = CONNECTION_OK;
    free_conn_params(&params);
    return conn;
}

void
PQfinish(PGconn *conn)
{
    if (conn == NULL)
        return;

    clear_pending_results(conn);
    if (conn->engine != NULL)
    {
        PGconn **entry_ptr;

        pthread_mutex_lock(&conn->engine->mutex);
        if (conn->engine->transaction_owner == conn
            && (conn->engine->transaction_status == 'T' || conn->engine->transaction_status == 'E'))
        {
            char *out_data = NULL;
            size_t out_len = 0;

            if (pglite_exec(conn->engine->db, "ROLLBACK", &out_data, &out_len) == 0)
            {
                engine_update_status(conn->engine, conn, out_data, out_len);
                pglite_free(out_data);
            }
            conn->engine->transaction_owner = NULL;
            conn->engine->transaction_status = 'I';
            pthread_cond_broadcast(&conn->engine->cond);
        }
        if (conn->notifyHead != NULL)
        {
            free_notify_queue(conn->notifyHead);
            conn->notifyHead = NULL;
            conn->notifyTail = NULL;
        }
        conn_listen_clear(conn);
        entry_ptr = &conn->engine->connections;
        while (*entry_ptr != NULL)
        {
            if (*entry_ptr == conn)
            {
                *entry_ptr = conn->engineNext;
                conn->engineNext = NULL;
                break;
            }
            entry_ptr = &(*entry_ptr)->engineNext;
        }
        pthread_mutex_unlock(&conn->engine->mutex);
        engine_detach(conn->engine);
    }

    close_notify_pipe(conn);
    free(conn->errorMessage);
    free(conn->dbname);
    free(conn->user);
    free(conn->password);
    free(conn->host);
    free(conn->port);
    free(conn->options);
    free(conn->application_name);
    free(conn->data_dir);
    free_parameters(conn->parameters);
    free(conn);
}

PQconninfoOption *
PQconninfoParse(const char *conninfo, char **errmsg)
{
    PGLiteConnParams params;
    PQconninfoOption *options;

    if (errmsg != NULL)
        *errmsg = NULL;

    if (!parse_conninfo_string(conninfo, &params, errmsg))
        return NULL;
    options = build_conninfo_options(&params);
    free_conn_params(&params);
    return options;
}

PQconninfoOption *
PQconninfo(PGconn *conn)
{
    PGLiteConnParams params;

    if (conn == NULL)
        return NULL;

    memset(&params, 0, sizeof(params));
    params.pglite_data_dir = conn->data_dir;
    params.dbname = conn->dbname;
    params.user = conn->user;
    params.password = conn->password;
    params.host = conn->host;
    params.port = conn->port;
    params.options = conn->options;
    params.application_name = conn->application_name;
    return build_conninfo_options(&params);
}

void
PQconninfoFree(PQconninfoOption *connOptions)
{
    size_t index;

    if (connOptions == NULL)
        return;

    for (index = 0; connOptions[index].keyword != NULL; index++)
        free(connOptions[index].val);
    free(connOptions);
}

char *
PQdb(const PGconn *conn)
{
    return (char *) nonnull_string(conn != NULL ? conn->dbname : NULL);
}

char *
PQuser(const PGconn *conn)
{
    return (char *) nonnull_string(conn != NULL ? conn->user : NULL);
}

char *
PQpass(const PGconn *conn)
{
    return (char *) nonnull_string(conn != NULL ? conn->password : NULL);
}

char *
PQhost(const PGconn *conn)
{
    return (char *) nonnull_string(conn != NULL ? conn->host : NULL);
}

char *
PQport(const PGconn *conn)
{
    return (char *) nonnull_string(conn != NULL ? conn->port : NULL);
}

char *
PQoptions(const PGconn *conn)
{
    return (char *) nonnull_string(conn != NULL ? conn->options : NULL);
}

ConnStatusType
PQstatus(const PGconn *conn)
{
    return conn != NULL ? conn->status : CONNECTION_BAD;
}

PGTransactionStatusType
PQtransactionStatus(const PGconn *conn)
{
    return conn != NULL ? conn->xactStatus : PQTRANS_UNKNOWN;
}

const char *
PQparameterStatus(const PGconn *conn, const char *paramName)
{
    if (conn == NULL || paramName == NULL)
        return NULL;
    return find_param_value(conn->parameters, paramName);
}

int
PQprotocolVersion(const PGconn *conn)
{
    return conn != NULL && conn->status == CONNECTION_OK ? 3 : 0;
}

int
PQserverVersion(const PGconn *conn)
{
    return conn != NULL ? conn->serverVersion : 0;
}

char *
PQerrorMessage(const PGconn *conn)
{
    return (char *) nonnull_string(conn != NULL ? conn->errorMessage : NULL);
}

int
PQsocket(const PGconn *conn)
{
    return conn != NULL ? conn->notifyReadFd : -1;
}

int
PQbackendPID(const PGconn *conn)
{
    return conn != NULL ? conn->backendPid : 0;
}

int
PQconnectionNeedsPassword(const PGconn *conn)
{
    (void) conn;
    return 0;
}

int
PQconnectionUsedPassword(const PGconn *conn)
{
    return conn != NULL && conn->usedPassword ? 1 : 0;
}

PQnoticeProcessor
PQsetNoticeProcessor(PGconn *conn, PQnoticeProcessor proc, void *arg)
{
    PQnoticeProcessor previous = NULL;

    if (conn == NULL)
        return NULL;

    previous = conn->noticeProcessor;
    conn->noticeProcessor = proc;
    conn->noticeProcessorArg = arg;
    return previous;
}

void
PQinitOpenSSL(int do_ssl, int do_crypto)
{
    (void) do_ssl;
    (void) do_crypto;
}

int
PQsslInUse(PGconn *conn)
{
    (void) conn;
    return 0;
}

const char *
PQsslAttribute(PGconn *conn, const char *attribute_name)
{
    (void) conn;
    if (attribute_name != NULL && strcmp(attribute_name, "library") == 0)
        return "pglite";
    return NULL;
}

const char *const *
PQsslAttributeNames(PGconn *conn)
{
    static const char *const names[] = { "library", NULL };

    (void) conn;
    return names;
}

PGcancel *
PQgetCancel(PGconn *conn)
{
    PGcancel *cancel = calloc(1, sizeof(*cancel));

    if (cancel != NULL)
        cancel->conn = conn;
    return cancel;
}

void
PQfreeCancel(PGcancel *cancel)
{
    free(cancel);
}

int
PQcancel(PGcancel *cancel, char *errbuf, int errbufsize)
{
    (void) cancel;
    if (errbuf != NULL && errbufsize > 0)
        errbuf[0] = '\0';
    return 1;
}

PGresult *
PQexec(PGconn *conn, const char *query)
{
    PGresult *head = NULL;
    PGresult *tail = NULL;
    PGresult *result;
    PGresult *last = NULL;

    if (conn == NULL || conn->status != CONNECTION_OK || query == NULL)
    {
        if (conn != NULL)
            set_conn_error(conn, "connection is not ready");
        return NULL;
    }

    if (!exec_query_to_results(conn, query, &head, &tail))
        return NULL;

    result = head;
    while (result != NULL)
    {
        PGresult *next = result->next;

        result->next = NULL;
        if (last != NULL)
            PQclear(last);
        last = result;
        result = next;
    }

    return last;
}

int
PQsendQuery(PGconn *conn, const char *query)
{
    PGresult *head = NULL;
    PGresult *tail = NULL;

    if (conn == NULL || conn->status != CONNECTION_OK || query == NULL)
    {
        if (conn != NULL)
            set_conn_error(conn, "connection is not ready");
        return 0;
    }

    if (conn->resultHead != NULL)
    {
        set_conn_error(conn, "another query is still pending");
        return 0;
    }

    if (!exec_query_to_results(conn, query, &head, &tail))
        return 0;

    conn->resultHead = head;
    conn->resultTail = tail;
    return 1;
}

PGresult *
PQgetResult(PGconn *conn)
{
    PGresult *result;

    if (conn == NULL || conn->resultHead == NULL)
        return NULL;

    result = conn->resultHead;
    conn->resultHead = result->next;
    if (conn->resultHead == NULL)
        conn->resultTail = NULL;
    result->next = NULL;
    return result;
}

int
PQconsumeInput(PGconn *conn)
{
    if (conn == NULL || conn->engine == NULL)
        return 0;

    pthread_mutex_lock(&conn->engine->mutex);
    drain_notify_pipe(conn);
    pthread_mutex_unlock(&conn->engine->mutex);
    return 1;
}

int
PQisBusy(PGconn *conn)
{
    (void) conn;
    return 0;
}

int
PQflush(PGconn *conn)
{
    (void) conn;
    return 0;
}

int
PQsetnonblocking(PGconn *conn, int arg)
{
    if (conn == NULL)
        return -1;
    conn->nonblocking = arg != 0;
    return 0;
}

PGnotify *
PQnotifies(PGconn *conn)
{
    PGnotify *event;

    if (conn == NULL || conn->engine == NULL)
        return NULL;

    pthread_mutex_lock(&conn->engine->mutex);
    event = conn->notifyHead;
    if (event != NULL)
    {
        conn->notifyHead = event->next;
        if (conn->notifyHead == NULL)
            conn->notifyTail = NULL;
        event->next = NULL;
    }
    pthread_mutex_unlock(&conn->engine->mutex);
    return event;
}

int
PQputCopyData(PGconn *conn, const char *buffer, int nbytes)
{
    (void) buffer;
    (void) nbytes;
    if (conn != NULL)
        set_conn_error(conn, "COPY protocol is not implemented yet in libpq-pglite");
    return -1;
}

int
PQputCopyEnd(PGconn *conn, const char *errormsg)
{
    (void) errormsg;
    if (conn != NULL)
        set_conn_error(conn, "COPY protocol is not implemented yet in libpq-pglite");
    return -1;
}

int
PQgetCopyData(PGconn *conn, char **buffer, int async)
{
    (void) async;
    if (buffer != NULL)
        *buffer = NULL;
    if (conn != NULL)
        set_conn_error(conn, "COPY protocol is not implemented yet in libpq-pglite");
    return -2;
}

ExecStatusType
PQresultStatus(const PGresult *res)
{
    return res != NULL ? res->resultStatus : PGRES_FATAL_ERROR;
}

char *
PQresStatus(ExecStatusType status)
{
    switch (status)
    {
        case PGRES_EMPTY_QUERY:
            return "PGRES_EMPTY_QUERY";
        case PGRES_COMMAND_OK:
            return "PGRES_COMMAND_OK";
        case PGRES_TUPLES_OK:
            return "PGRES_TUPLES_OK";
        case PGRES_COPY_OUT:
            return "PGRES_COPY_OUT";
        case PGRES_COPY_IN:
            return "PGRES_COPY_IN";
        case PGRES_BAD_RESPONSE:
            return "PGRES_BAD_RESPONSE";
        case PGRES_NONFATAL_ERROR:
            return "PGRES_NONFATAL_ERROR";
        case PGRES_FATAL_ERROR:
            return "PGRES_FATAL_ERROR";
        case PGRES_COPY_BOTH:
            return "PGRES_COPY_BOTH";
        case PGRES_SINGLE_TUPLE:
            return "PGRES_SINGLE_TUPLE";
        case PGRES_PIPELINE_SYNC:
            return "PGRES_PIPELINE_SYNC";
        case PGRES_PIPELINE_ABORTED:
            return "PGRES_PIPELINE_ABORTED";
        case PGRES_TUPLES_CHUNK:
            return "PGRES_TUPLES_CHUNK";
        default:
            return "PGRES_UNKNOWN";
    }
}

char *
PQresultErrorMessage(const PGresult *res)
{
    return (char *) nonnull_string(res != NULL ? res->errorMessage : NULL);
}

char *
PQresultErrorField(const PGresult *res, int fieldcode)
{
    if (res == NULL || fieldcode < 0 || fieldcode > 255)
        return NULL;
    return res->errorFields[fieldcode];
}

int
PQntuples(const PGresult *res)
{
    return res != NULL ? res->ntuples : 0;
}

int
PQnfields(const PGresult *res)
{
    return res != NULL ? res->nfields : 0;
}

int
PQbinaryTuples(const PGresult *res)
{
    return res != NULL ? res->binary : 0;
}

char *
PQfname(const PGresult *res, int field_num)
{
    if (res == NULL || field_num < 0 || field_num >= res->nfields)
        return NULL;
    return res->attDescs[field_num].name;
}

Oid
PQftable(const PGresult *res, int field_num)
{
    if (res == NULL || field_num < 0 || field_num >= res->nfields)
        return InvalidOid;
    return res->attDescs[field_num].tableid;
}

int
PQftablecol(const PGresult *res, int field_num)
{
    if (res == NULL || field_num < 0 || field_num >= res->nfields)
        return 0;
    return res->attDescs[field_num].columnid;
}

Oid
PQftype(const PGresult *res, int field_num)
{
    if (res == NULL || field_num < 0 || field_num >= res->nfields)
        return InvalidOid;
    return res->attDescs[field_num].typid;
}

int
PQfsize(const PGresult *res, int field_num)
{
    if (res == NULL || field_num < 0 || field_num >= res->nfields)
        return 0;
    return res->attDescs[field_num].typlen;
}

int
PQfmod(const PGresult *res, int field_num)
{
    if (res == NULL || field_num < 0 || field_num >= res->nfields)
        return -1;
    return res->attDescs[field_num].atttypmod;
}

char *
PQcmdStatus(PGresult *res)
{
    return (char *) nonnull_string(res != NULL ? res->cmdStatus : NULL);
}

Oid
PQoidValue(const PGresult *res)
{
    return res != NULL ? res->oidValue : InvalidOid;
}

char *
PQcmdTuples(PGresult *res)
{
    return (char *) nonnull_string(res != NULL ? res->cmdTuples : NULL);
}

char *
PQgetvalue(const PGresult *res, int tup_num, int field_num)
{
    int cell_index;

    if (res == NULL || tup_num < 0 || tup_num >= res->ntuples || field_num < 0 || field_num >= res->nfields)
        return (char *) empty_string;

    cell_index = tup_num * res->nfields + field_num;
    if (res->cells[cell_index].isnull || res->cells[cell_index].value == NULL)
        return (char *) empty_string;
    return res->cells[cell_index].value;
}

int
PQgetlength(const PGresult *res, int tup_num, int field_num)
{
    int cell_index;

    if (res == NULL || tup_num < 0 || tup_num >= res->ntuples || field_num < 0 || field_num >= res->nfields)
        return 0;

    cell_index = tup_num * res->nfields + field_num;
    return res->cells[cell_index].isnull ? 0 : res->cells[cell_index].len;
}

int
PQgetisnull(const PGresult *res, int tup_num, int field_num)
{
    int cell_index;

    if (res == NULL || tup_num < 0 || tup_num >= res->ntuples || field_num < 0 || field_num >= res->nfields)
        return 1;

    cell_index = tup_num * res->nfields + field_num;
    return res->cells[cell_index].isnull ? 1 : 0;
}

void
PQclear(PGresult *res)
{
    free_result(res);
}

void
PQfreemem(void *ptr)
{
    free(ptr);
}

size_t
PQescapeStringConn(PGconn *conn, char *to, const char *from, size_t length, int *error)
{
    size_t written = 0;
    size_t index;
    bool escape_backslash = false;

    if (error != NULL)
        *error = 0;

    if (conn != NULL)
    {
        const char *scs = PQparameterStatus(conn, "standard_conforming_strings");
        escape_backslash = !(scs != NULL && strcmp(scs, "on") == 0);
    }

    for (index = 0; index < length; index++)
    {
        if (SQL_STR_DOUBLE(from[index], escape_backslash))
            to[written++] = from[index];
        to[written++] = from[index];
    }
    to[written] = '\0';
    return written;
}

char *
PQescapeIdentifier(PGconn *conn, const char *str, size_t len)
{
    char *result;
    size_t index;
    size_t written = 0;

    (void) conn;
    result = malloc(len * 2 + 3);
    if (result == NULL)
        return NULL;

    result[written++] = '"';
    for (index = 0; index < len; index++)
    {
        if (str[index] == '"')
            result[written++] = '"';
        result[written++] = str[index];
    }
    result[written++] = '"';
    result[written] = '\0';
    return result;
}

unsigned char *
PQescapeByteaConn(PGconn *conn, const unsigned char *from, size_t from_length, size_t *to_length)
{
    unsigned char *result;
    static const char hex[] = "0123456789abcdef";
    size_t index;
    size_t written = 0;

    (void) conn;
    result = malloc(from_length * 2 + 3);
    if (result == NULL)
        return NULL;

    result[written++] = '\\';
    result[written++] = 'x';
    for (index = 0; index < from_length; index++)
    {
        result[written++] = (unsigned char) hex[(from[index] >> 4) & 0x0F];
        result[written++] = (unsigned char) hex[from[index] & 0x0F];
    }
    result[written] = '\0';
    if (to_length != NULL)
        *to_length = written;
    return result;
}

size_t
PQescapeString(char *to, const char *from, size_t length)
{
    return PQescapeStringConn(NULL, to, from, length, NULL);
}

unsigned char *
PQescapeBytea(const unsigned char *from, size_t from_length, size_t *to_length)
{
    return PQescapeByteaConn(NULL, from, from_length, to_length);
}

int
PQlibVersion(void)
{
    return PG_VERSION_NUM;
}

char *
PQencryptPassword(const char *passwd, const char *user)
{
    char *buffer;
    const char *errstr = NULL;

    if (passwd == NULL || user == NULL)
        return NULL;

    buffer = malloc(MD5_PASSWD_LEN + 1);
    if (buffer == NULL)
        return NULL;

    if (!pg_md5_encrypt(passwd, user, strlen(user), buffer, &errstr))
    {
        free(buffer);
        return NULL;
    }
    return buffer;
}

char *
PQencryptPasswordConn(PGconn *conn, const char *passwd, const char *user, const char *algorithm)
{
    if (algorithm == NULL || strcmp(algorithm, "md5") == 0 || strcmp(algorithm, "on") == 0 || strcmp(algorithm, "off") == 0)
        return PQencryptPassword(passwd, user);

    if (conn != NULL)
        set_conn_error(conn, "password encryption algorithm \"%s\" is not supported", algorithm);
    return NULL;
}

int
lo_open(PGconn *conn, Oid lobjId, int mode)
{
    (void) lobjId;
    (void) mode;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return -1;
}

int
lo_close(PGconn *conn, int fd)
{
    (void) fd;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return -1;
}

int
lo_read(PGconn *conn, int fd, char *buf, size_t len)
{
    (void) fd;
    (void) buf;
    (void) len;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return -1;
}

int
lo_write(PGconn *conn, int fd, const char *buf, size_t len)
{
    (void) fd;
    (void) buf;
    (void) len;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return -1;
}

pg_int64
lo_lseek64(PGconn *conn, int fd, pg_int64 offset, int whence)
{
    (void) fd;
    (void) offset;
    (void) whence;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return -1;
}

int
lo_lseek(PGconn *conn, int fd, int offset, int whence)
{
    return (int) lo_lseek64(conn, fd, offset, whence);
}

pg_int64
lo_tell64(PGconn *conn, int fd)
{
    (void) fd;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return -1;
}

int
lo_tell(PGconn *conn, int fd)
{
    return (int) lo_tell64(conn, fd);
}

int
lo_truncate64(PGconn *conn, int fd, pg_int64 len)
{
    (void) fd;
    (void) len;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return -1;
}

int
lo_truncate(PGconn *conn, int fd, size_t len)
{
    return lo_truncate64(conn, fd, (pg_int64) len);
}

Oid
lo_creat(PGconn *conn, int mode)
{
    (void) mode;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return InvalidOid;
}

Oid
lo_create(PGconn *conn, Oid lobjId)
{
    (void) lobjId;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return InvalidOid;
}

int
lo_unlink(PGconn *conn, Oid lobjId)
{
    (void) lobjId;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return -1;
}

Oid
lo_import(PGconn *conn, const char *filename)
{
    (void) filename;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return InvalidOid;
}

int
lo_export(PGconn *conn, Oid lobjId, const char *filename)
{
    (void) lobjId;
    (void) filename;
    if (conn != NULL)
        set_conn_error(conn, "large objects are not implemented yet in libpq-pglite");
    return -1;
}
