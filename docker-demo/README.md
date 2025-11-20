# SheetReader DuckDB Docker Demo

Demo of the **sheetreader-duckdb** extension running in a Docker container to safely query Excel files with SQL.

## Prerequisites

- Docker Desktop must be running

## Quick Start (Interactive Mode)

### Step 1: Navigate to the demo directory
```bash
cd c:\Users\mithu\OneDrive\Desktop\SA_Harry\SheetReader\docker-demo
```

### Step 2: Build and start DuckDB (first time only)
```bash
docker-compose build
```

### Step 3: Start DuckDB
```bash
docker-compose run --rm duckdb
```

### Step 4: Inside DuckDB, run SQL commands
```sql
-- Install and load the extension
INSTALL sheetreader FROM community;
LOAD sheetreader;

-- Query your Excel file
SELECT * FROM sheetreader('test.xlsx');

-- Get statistics
SELECT 
    MIN(Numeric0) as min_value,
    MAX(Numeric0) as max_value,
    AVG(Numeric0) as avg_value,
    SUM(Numeric0) as sum_value
FROM sheetreader('test.xlsx');

-- Create a table
CREATE TABLE excel_data AS FROM sheetreader('test.xlsx');

-- Filter data
SELECT * FROM excel_data WHERE Numeric0 > 50;
```

### Step 5: Exit when done
```
.exit
```

---

## Run the Full Demo Script (Automated)

To run all queries automatically:

```bash
docker-compose run --rm duckdb -init demo.sql
```

---

## Files

- **Dockerfile** - Container setup with DuckDB
- **docker-compose.yml** - Docker configuration (includes volume mounts)
- **demo.sql** - Demo SQL script
- **test.xlsx** - Sample Excel file with 5 random numbers
