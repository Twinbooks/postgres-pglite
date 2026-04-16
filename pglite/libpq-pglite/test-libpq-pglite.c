#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libpq-fe.h"

static void
die(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void
check_status(PGconn *conn)
{
    if (PQstatus(conn) != CONNECTION_OK)
    {
        fprintf(stderr, "connection failed: %s\n", PQerrorMessage(conn));
        exit(1);
    }
}

static void
check_result(PGresult *res, ExecStatusType expected, const char *label)
{
    if (res == NULL)
    {
        fprintf(stderr, "%s returned NULL\n", label);
        exit(1);
    }
    if (PQresultStatus(res) != expected)
    {
        fprintf(stderr, "%s failed: %s\n", label, PQresultErrorMessage(res));
        exit(1);
    }
}

int
main(void)
{
    char data_dir_template[] = "/tmp/libpq-pglite-smoke-XXXXXX";
    char conninfo[1024];
    PGconn *conn;
    PGresult *res;
    char *data_dir;
    const char *json_text = "{\"en_US\": \"This introduces a \\\"release to pay\\\" mechanism and don't break.\"}";
    char escaped_json[1024];
    char insert_json_sql[1200];
    int escape_error = 0;

    data_dir = mkdtemp(data_dir_template);
    if (data_dir == NULL)
        die("mkdtemp failed");

    snprintf(conninfo, sizeof(conninfo), "pglite_data_dir=%s", data_dir);
    conn = PQconnectdb(conninfo);
    if (conn == NULL)
        die("PQconnectdb returned NULL");

    check_status(conn);

    res = PQexec(conn, "create table demo (id int primary key, name text)");
    check_result(res, PGRES_COMMAND_OK, "create table");
    PQclear(res);

    res = PQexec(conn, "insert into demo values (1, 'alpha')");
    check_result(res, PGRES_COMMAND_OK, "insert");
    if (strcmp(PQcmdTuples(res), "1") != 0)
        die("insert rowcount mismatch");
    PQclear(res);

    res = PQexec(conn, "begin");
    check_result(res, PGRES_COMMAND_OK, "begin");
    PQclear(res);
    if (PQtransactionStatus(conn) != PQTRANS_INTRANS)
        die("transaction status did not switch to INTRANS");

    res = PQexec(conn, "rollback");
    check_result(res, PGRES_COMMAND_OK, "rollback");
    PQclear(res);
    if (PQtransactionStatus(conn) != PQTRANS_IDLE)
        die("transaction status did not switch back to IDLE");

    res = PQexec(conn, "select id, name from demo order by id");
    check_result(res, PGRES_TUPLES_OK, "select");
    if (PQntuples(res) != 1 || PQnfields(res) != 2)
        die("unexpected result shape");
    if (strcmp(PQfname(res, 0), "id") != 0 || strcmp(PQfname(res, 1), "name") != 0)
        die("unexpected column names");
    if (strcmp(PQgetvalue(res, 0, 0), "1") != 0 || strcmp(PQgetvalue(res, 0, 1), "alpha") != 0)
        die("unexpected row contents");
    PQclear(res);

    if (PQescapeStringConn(conn, escaped_json, json_text, strlen(json_text), &escape_error) == 0 && json_text[0] != '\0')
        die("json escaping unexpectedly returned zero length");
    if (escape_error != 0)
        die("json escaping reported an error");
    if (strstr(escaped_json, "\\\\\"") != NULL)
        die("json escaping incorrectly doubled a backslash before quotes");

    res = PQexec(conn, "create table json_demo (payload json)");
    check_result(res, PGRES_COMMAND_OK, "create json table");
    PQclear(res);

    snprintf(insert_json_sql, sizeof(insert_json_sql),
             "insert into json_demo (payload) values ('%s')", escaped_json);
    res = PQexec(conn, insert_json_sql);
    check_result(res, PGRES_COMMAND_OK, "insert escaped json");
    PQclear(res);

    res = PQexec(conn, "select payload->>'en_US' from json_demo");
    check_result(res, PGRES_TUPLES_OK, "select escaped json");
    if (PQntuples(res) != 1 || strcmp(PQgetvalue(res, 0, 0),
        "This introduces a \"release to pay\" mechanism and don't break.") != 0)
        die("unexpected escaped json contents");
    PQclear(res);

    res = PQexec(conn, "selekt 1");
    check_result(res, PGRES_FATAL_ERROR, "syntax error");
    if (PQresultErrorField(res, PG_DIAG_SQLSTATE) == NULL)
        die("fatal error did not preserve SQLSTATE");
    PQclear(res);

    if (PQserverVersion(conn) <= 0)
        die("server version probe failed");
    if (PQprotocolVersion(conn) != 3)
        die("protocol version mismatch");

    PQfinish(conn);
    printf("libpq-pglite smoke test passed\n");
    return 0;
}
