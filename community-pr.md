Hi!

In the last semester, I was part of a programming project organized by the DIMA group at TU Berlin. We created a small DuckDB-extension named [`sheetreader`](https://github.com/polydbms/sheetreader-duckdb) that utilizes [sheetreader-core](https://github.com/polydbms/sheetreader-core) (a fast multi-threaded XLSX parser) for importing XLSX files into DuckDB.

We did a few benchmarks comparing our extension to the import function which the `spatial` extension provides (`st_read`). After our first benchmarks we came to the conclusion, our extension is around 5 to 10 times faster than the `spatial` extension at parsing XLSX files and loading them into DuckDB.

We would like to offer this extension as a DuckDB community extension.

A note regarding the repository structure of our extension:
- We have a version in branch [`benchmark-version`](https://github.com/polydbms/sheetreader-duckdb/tree/benchmark-version) that has code dedicated for benchmarking.
- On the branch [`main`](https://github.com/polydbms/sheetreader-duckdb/tree/main), we provide a "slimmed down" version with that code removed. The latter is the version we would like to offer as a community extension.
