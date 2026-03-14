-- SheetReader DuckDB Extension Demo (Release Install)
-- This script demonstrates how to install the extension directly from a GitHub Release URL

-- Step 1: Download and rename the extension (since DuckDB INSTALL requires the filename to match)
-- We use the linux_amd64 version for this Docker environment
.shell wget -O sheetreader.duckdb_extension.gz https://github.com/polydbms/sheetreader-duckdb/releases/download/v0.2.0/sheetreader-ext-v0.2.0-for-duckdb-v1.5.0-linux_amd64.duckdb_extension.gz
.shell gunzip -f sheetreader.duckdb_extension.gz

-- Step 2: Install and Load
INSTALL 'sheetreader.duckdb_extension';
LOAD sheetreader;

-- Step 3: Query the Excel file
-- NOTE: If you see "Inline & dynamic String types not supported yet", 
-- it means your Excel file uses a specific XLSX feature called "Inline Strings" 
-- which is currently a "TODO" in the SheetReader extension source code.
.print '=== Reading /workspace/test.xlsx with Release binary ==='
SELECT * FROM sheetreader('/workspace/test.xlsx');

.print ''
.print '=== Demo completed successfully! ==='
