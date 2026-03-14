-- SheetReader DuckDB Extension Demo (Source Install)
-- This script demonstrates how to use the sheetreader extension built from source

-- Step 1: Load the locally built extension
-- We assume the extension was built in the host's build/release directory
.load '../build/release/extension/sheetreader/sheetreader.duckdb_extension'

-- Step 2: Query the Excel file directly
.print '=== Reading test.xlsx with locally built sheetreader ==='
SELECT * FROM sheetreader('test.xlsx');

-- Step 3: Get row count
.print ''
.print '=== Row count ==='
SELECT COUNT(*) as total_rows FROM sheetreader('test.xlsx');

.print ''
.print '=== Demo completed successfully! ==='
