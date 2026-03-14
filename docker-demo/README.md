# SheetReader DuckDB Docker Sandbox

This directory provides an isolated environment to test the **sheetreader-duckdb** extension. It includes pre-configured tools for building from source For a quick, clean trial using the **v0.2.0 binaries**:

## Usage Guide

The following commands assume you are inside the `docker-demo/` directory.

### 1. Test Official Release (v0.2.0)
This is the fastest way to try the extension. It downloads the pre-compiled binary directly from GitHub.

```bash
docker compose run --rm sheetreader-dev duckdb -unsigned -c ".read /workspace/demo_release.sql"
```

### 2. Test Community Repository
Simulate the end-user experience of installing via DuckDB's community index.

```bash
docker compose run --rm sheetreader-dev duckdb -c ".read /workspace/demo_community.sql"
```

### 3. Build & Develop from Source
For contributors wanting to test local modifications in a clean environment.

```bash
# Start background environment
docker compose up -d

# Enter container
docker compose exec sheetreader-dev bash

# Build & Run (Inside container)
GEN=ninja make release
duckdb -c ".read demo_source.sql"
```

## Expected Execution Result

Upon success, the SQL demos will query the local `test.xlsx` and display:

```text
┌─────────┬────────┬──────────┐
│  Name   │  Age   │   City   │
│ varchar │ double │ varchar  │
├─────────┼────────┼──────────┤
│ Alice   │   30.0 │ New York │
│ Bob     │   25.0 │ London   │
└─────────┴────────┴──────────┘

=== Demo completed successfully! ===
```
