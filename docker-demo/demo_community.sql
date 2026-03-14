-- SheetReader DuckDB Extension Demo (Community Install)
-- This script demonstrates how to install the sheetreader extension from the community repository

-- Step 1: Install the sheetreader extension from community extensions
INSTALL sheetreader FROM community;

-- Step 2: Load the extension
LOAD sheetreader;

-- Step 3: Query the Excel file directly
.print '=== Reading test.xlsx with sheetreader ==='
SELECT * FROM sheetreader('test.xlsx');

-- Step 4: Get row count
.print ''
.print '=== Row count ==='
SELECT COUNT(*) as total_rows FROM sheetreader('test.xlsx');

.print ''
.print '=== Demo completed successfully! ==='
