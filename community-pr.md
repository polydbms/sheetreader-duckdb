Hi!

In the last semester, we created a small DuckDB-extension named "sheetreader-duckdb" that uses [sheetreader-core]() (a fast multi-threaded XLSX parser) for importing XLSX files into DuckDB.

We did a few benchmarks in comparison to the import function which the `spatial` extension provides.

Depending on the number of threads and other properties of the CPU, our extension is around 5 times faster than the `spatial` extension.

We would like to offer this extension as a DuckDB community extension.

A few notes:
- We had issues with our build pipeline. When loading the build into DuckDB, we got a "metadata mismatch" error. The issue we had is documented in this issue: .... For the time being, we truncated the metadata from the binary manually to circumvent the issue.
  This is not part of our build pipeline though and was only done after downloading the binary from the AWS S3 bucket.
- We have a version in [`main`](???) that has code dedicated for benchmarking. On the branch [`community-version`](???), we provide a "slimmed down" with that code removed.
- This is our main repository:
  We have second repository with the CI properly set up:
- dllimport
https://github.com/duckdb/duckdb/issues/4144
https://github.com/marcboeker/go-duckdb/issues/51