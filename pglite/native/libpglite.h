#ifndef LIBPGLITE_H
#define LIBPGLITE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PGlite PGlite;

int pglite_open(const char *data_dir, PGlite **out_db);
int pglite_open_with_options(const char *data_dir, const char *bootstrap_mode, PGlite **out_db);
int pglite_exec(PGlite *db, const char *sql, char **out_data, size_t *out_len);
int pglite_exec_protocol(
    PGlite *db,
    const char *message,
    size_t message_len,
    char **out_data,
    size_t *out_len
);
int pglite_close(PGlite *db);

const char *pglite_error(const PGlite *db);
const char *pglite_global_error(void);
void pglite_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
