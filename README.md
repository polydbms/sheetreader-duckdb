# Sheetreader DuckDB extension

This DuckDB extension, Sheetreader, allows you to read .XLSX files by using https://github.com/polydbms/sheetreader-core.

---

This repository is based on https://github.com/duckdb/extension-template.

### Installing pre-built binaries
To install your extension binaries from S3, you will need to do two things. Firstly, DuckDB should be launched with the
`allow_unsigned_extensions` option set to true. How to set this will depend on the client you're using. Some examples:

CLI:
```shell
duckdb -unsigned
```

Python:
```python
con = duckdb.connect(':memory:', config={'allow_unsigned_extensions' : 'true'})
```

NodeJS:
```js
db = new duckdb.Database(':memory:', {"allow_unsigned_extensions": "true"});
```

Get the extension from S3 (platform is either `linux_amd64`, `linux_amd64_gcc4`, `linux_arm64`, `osx_arm64`, `osx_amd64`, `windows_amd64`, `wasm_eh`, `wasm_mvp`, `wasm_threads`):

```
wget https://duckdb-sheetreader-extension.s3.eu-central-1.amazonaws.com/v1.0.0/<platform>/sheetreader.duckdb_extension.gz
```


At the moment the metadata mechanic doesn't work, so you have to prepare the extension for loading:

```bash
gzip -d sheetreader.duckdb_extension.gz
truncate -s -256 sheetreader.duckdb_extension # Delete metadata
```

<!-- Secondly, you will need to set the repository endpoint in DuckDB to the HTTP url of your bucket + version of the extension -->
<!-- you want to install. To do this run the following SQL query in DuckDB: -->
<!-- ```sql
SET custom_extension_repository='bucket.s3.eu-west-1.amazonaws.com/<your_extension_name>/latest';
```
Note that the `/latest` path will allow you to install the latest extension version available for your current version of
DuckDB. To specify a specific version, you can pass the version instead. -->

After running these steps, you can install and load the extension using the regular `INSTALL`/`LOAD`commands in DuckDB:
```sql
D FORCE INSTALL './sheetreader.duckdb_extension';
D LOAD sheetreader;
```

Now we can use the features from the extension directly in DuckDB. The extension contains a table function `sheetreader()` that takes the path of an .XLSX file and returns a table:
```sql
D from sheetreader('data.xlsx',threads=4);
```

## Building yourself

To build the extension, run:
```sh
GEN=ninja make
```
The main binaries that will be built are:
```sh
./build/release/duckdb
./build/release/extension/sheetreader/sheetreader.duckdb_extension
```
- `duckdb` is the binary for the duckdb shell with the extension code automatically loaded.
- `sheetreader.duckdb_extension` is the loadable binary as it would be distributed.

### Running the extension

To run the extension code, simply start the shell with `./build/release/duckdb`.
