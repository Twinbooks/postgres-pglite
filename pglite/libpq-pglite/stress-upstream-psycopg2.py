from concurrent.futures import ThreadPoolExecutor
import os
import shutil
from pathlib import Path

import psycopg2


THREADS = int(os.environ.get("PGLITE_STRESS_THREADS", "4"))
LOOPS = int(os.environ.get("PGLITE_STRESS_LOOPS", "40"))
DATA_DIR = Path(os.environ["DATA_DIR"])


def open_connection(iteration: int):
    if iteration % 3 == 0:
        return psycopg2.connect(pglite_data_dir=str(DATA_DIR))
    if iteration % 3 == 1:
        return psycopg2.connect(str(DATA_DIR))
    return psycopg2.connect(DATA_DIR)


def worker(worker_id: int) -> None:
    for iteration in range(LOOPS):
        with open_connection(iteration) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "select current_setting('server_version_num'), current_setting('server_version')"
                )
                version_row = cur.fetchone()
                assert len(version_row) == 2, version_row
                assert version_row[0].isdigit(), version_row
                assert version_row[1], version_row

                cur.execute("select %s, %s", (worker_id, iteration))
                row = cur.fetchone()
                assert row == (worker_id, iteration), row


def main() -> None:
    shutil.rmtree(DATA_DIR, ignore_errors=True)

    with psycopg2.connect(pglite_data_dir=str(DATA_DIR)) as conn:
        with conn.cursor() as cur:
            cur.execute("select 1")
            assert cur.fetchone() == (1,)

    with ThreadPoolExecutor(max_workers=THREADS) as executor:
        futures = [executor.submit(worker, worker_id) for worker_id in range(THREADS)]
        for future in futures:
            future.result()

    print(f"upstream psycopg2 stress test passed ({THREADS} threads x {LOOPS} loops)")


if __name__ == "__main__":
    main()
