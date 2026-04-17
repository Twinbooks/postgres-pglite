import json
import os
from pathlib import Path
import selectors

import psycopg2
from psycopg2.extras import Json


def main() -> None:
    data_dir = Path(os.environ["DATA_DIR"])

    conn = psycopg2.connect(f"pglite_data_dir={data_dir}")
    conn.close()
    conn = psycopg2.connect(data_dir)
    conn.close()
    conn = psycopg2.connect(pglite_data_dir=data_dir)
    conn.close()

    conn = psycopg2.connect(f"pglite_data_dir={data_dir}")
    cur = conn.cursor()
    cur.execute("create table demo (id int primary key, name text)")
    cur.execute("insert into demo values (1, 'alpha')")
    cur.execute("select id, name from demo")
    rows = cur.fetchall()
    assert rows == [(1, "alpha")], rows
    conn.commit()

    conn2 = psycopg2.connect(f"pglite_data_dir={data_dir}")
    cur2 = conn2.cursor()
    cur2.execute("select count(*) from demo")
    assert cur2.fetchall() == [(1,)], cur2.fetchall()
    cur2.close()

    cur.execute("create table demo_json (payload json)")
    payload = {"en_US": 'This introduces a "release to pay" mechanism and don\'t break.'}
    json_text = json.dumps(payload)
    cur.execute("insert into demo_json (payload) values (%s)", (json_text,))
    cur.execute("insert into demo_json (payload) values (%s)", (Json(payload),))
    cur.execute("select payload->>'en_US' from demo_json order by 1")
    json_rows = cur.fetchall()
    assert json_rows == [
        ('This introduces a "release to pay" mechanism and don\'t break.',),
        ('This introduces a "release to pay" mechanism and don\'t break.',),
    ], json_rows

    listen_cur = conn.cursor()
    listen_cur.execute("listen imbus")
    conn.commit()
    assert conn.fileno() >= 0, conn.fileno()

    with selectors.DefaultSelector() as selector:
        selector.register(conn, selectors.EVENT_READ)
        cur2 = conn2.cursor()
        cur2.execute("select pg_notify('imbus', %s)", ('["demo-channel"]',))
        conn2.commit()
        ready = selector.select(timeout=5)
        assert ready, "LISTEN/NOTIFY selector did not become readable"
        conn.poll()
        payloads = [notify.payload for notify in conn.notifies]
        assert payloads == ['["demo-channel"]'], payloads
        conn.notifies.clear()
        cur2.close()

    conn2.close()
    listen_cur.close()
    cur.close()
    conn.close()

    print("upstream psycopg2 smoke test passed")


if __name__ == "__main__":
    main()
