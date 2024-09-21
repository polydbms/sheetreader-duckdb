Hi!

In the last semester, I was part of a programming project organized by the DIMA group at TU Berlin. We created a small DuckDB-extension named "sheetreader-duckdb" that utilizes [sheetreader-core](https://github.com/polydbms/sheetreader-core) (a fast multi-threaded XLSX parser) for importing XLSX files into DuckDB.

We did a few benchmarks comparing our extension to the import function which the `spatial` extension provides (`st_read`). Depending on the number of threads and other properties of the CPU, our extension is around 5 times faster than the `spatial` extension.

We would like to offer this extension as a DuckDB community extension.

These are issues that have to be resolved first, though:
- We have a build pipeline set up (using https://github.com/duckdb/extension-ci-tools) & the builds are successful for every platform except `windows_amd64_rtools` (as you can see here: https://github.com/polydbms/sheetreader-duckdb/actions/runs/10971586308/job/30466877905#step:15:1293).
  There is an issue with functions marked as `dllimport`. Unfortunately, I don't have experience with MinGW and it doesn't seem to be related to our code.
  I found these two issues where people has similar issues: https://github.com/duckdb/duckdb/issues/4144#issuecomment-1264625149 & https://github.com/marcboeker/go-duckdb/issues/51#issuecomment-1859057708.
- We had issues with the builds created by the CI and uploaded to S3. When loading the builds into DuckDB, we got a "metadata mismatch" error. The issue we had is documented in this issue: https://github.com/duckdb/extension-template/issues/70. For the time being, we truncated the metadata from the binary manually to circumvent the issue.
  Maybe this issue resolves itself when it's build by your pipeline?

A few notes:
- We have a version in branch [`benchmark-version`](https://github.com/polydbms/sheetreader-duckdb/tree/benchmark-version) that has code dedicated for benchmarking. On the branch [`main`](https://github.com/polydbms/sheetreader-duckdb/tree/main), we provide a "slimmed down" version with that code removed. The latter is the version we would like to offer as a community extension.
- We have a second repository with the CI also uploading the builds to an S3 bucket: https://github.com/freddie-freeloader/duckdb-extension-test
