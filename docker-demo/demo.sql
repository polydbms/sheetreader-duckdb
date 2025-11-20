-- SheetReader DuckDB Extension Demo
-- This script demonstrates how to use the sheetreader extension to query Excel files

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

-- Step 5: Calculate statistics on the data
.print ''
.print '=== Statistics ==='
SELECT 
    MIN(Numeric0) as min_value,
    MAX(Numeric0) as max_value,
    AVG(Numeric0) as avg_value,
    SUM(Numeric0) as sum_value
FROM sheetreader('test.xlsx');

-- Step 6: Create a table from the Excel data
.print ''
.print '=== Creating table from Excel data ==='
CREATE TABLE excel_data AS 
FROM sheetreader('test.xlsx');

-- Step 7: Query the created table
.print ''
.print '=== Querying the created table ==='
SELECT * FROM excel_data;

-- Step 8: Filter data (example: values greater than 50)
.print ''
.print '=== Filtering values > 50 ==='
SELECT * FROM excel_data WHERE Numeric0 > 50;

.print ''
.print '=== Demo completed successfully! ==='
