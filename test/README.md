# Testing this extension

Tests live under `test/` and use DuckDB’s [SQLLogicTest](https://duckdb.org/dev/sqllogictest/intro.html) format (`.test` files).

These are **DuckDB integration tests**, not unit tests of sheetreader-core alone. Each test:

1. Starts with `require sheetreader` so the extension is loaded (or the test is skipped)
2. Runs SQL against the `sheetreader()` table function
3. Indirectly exercises sheetreader-core through that API

Parser-only / core-only coverage belongs in [sheetreader-core](https://github.com/polydbms/sheetreader-core). This repo focuses on the DuckDB binding: types, headers, parameters, errors, and result materialization.

## Layout

```
test/
  sql/
    basic.test          # header auto-detect, describe, counts
    parameters.test     # sheet_name/index, skip_rows, threads, …
    types.test          # types / coerce_to_string / force_types
    errors.test         # invalid parameter combinations
    data/               # small .xlsx fixtures checked into git
```

Paths in tests are relative to the extension repo root (e.g. `test/sql/data/people.xlsx`).

## Running

Build the extension, then run the suite:

```bash
GEN=ninja make release
make test
```

Or a single file:

```bash
./build/release/test/unittest test/sql/basic.test
```
