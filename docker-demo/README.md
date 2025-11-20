# SheetReader DuckDB Docker Demo

Demo of the **sheetreader-duckdb** extension with DuckDB v1.4.0+ compatibility.

## Prerequisites

- Docker and Docker Compose installed and running

## DuckDB v1.4.0 Extension Verification

This setup allows you to verify that the sheetreader extension works correctly with DuckDB v1.4.0+.

### Build and Test the Extension

**Step 1: Navigate to the demo directory**
```bash
cd docker-demo
```

**Step 2: Build the Docker image**
```bash
docker compose build
```

**Step 3: Build the extension from source**
```bash
docker compose run --rm sheetreader-dev bash -c "GEN=ninja make"
```

This will:
- Build DuckDB v1.4.0+ from source
- Compile the sheetreader extension with the new API
- Create a DuckDB binary with the extension pre-loaded

**Step 4: Run the verification test**
```bash
docker compose run --rm sheetreader-dev bash -c "./build/release/duckdb < docker-demo/test_verification.sql"
```

**Expected output:**
```
┌──────────┐
│ Numeric0 │
│  double  │
├──────────┤
│     92.0 │
│     48.0 │
│     99.0 │
│     35.0 │
│     97.0 │
└──────────┘
```

If you see this output, the extension is working correctly with DuckDB v1.4.0+! ✅

---

## Interactive Development

For interactive development and testing:

**Start an interactive shell:**
```bash
docker compose run --rm sheetreader-dev bash
```

**Inside the container, you can:**
```bash
# Build the extension
GEN=ninja make


# start DuckDB interactively
./build/release/duckdb
```

**Inside DuckDB, try queries:**
```sql
-- Query the Excel file
SELECT * FROM sheetreader('docker-demo/test.xlsx');

```

**Exit:**
```
.exit  # Exit DuckDB
exit   # Exit container
```

---

## Files

- **Dockerfile** - Ubuntu 22.04 with build dependencies (git, cmake, ninja, etc.)
- **docker-compose.yml** - Docker Compose setup with volume mounts and ccache
- **test.xlsx** - Sample Excel file with test data
- **test_verification.sql** - Verification query for testing
