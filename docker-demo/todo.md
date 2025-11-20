# Ubuntu Verification Tasks

## Context
The `sheetreader-duckdb` extension has been updated for DuckDB v1.4.0 compatibility.
The code changes are applied in `src/` (migrated to `ExtensionLoader` API).
Verification is required on an Ubuntu machine.

## Tasks

1.  **Install Dependencies**
    Ensure the following packages are installed:
    ```bash
    sudo apt-get update && sudo apt-get install -y git cmake build-essential ninja-build libssl-dev python3-dev
    ```

2.  **Build Extension**
    Build the extension from source using `ninja`:
    ```bash
    # In the sheetreader-duckdb directory
    GEN=ninja make
    ```

3.  **Verify Functionality**
    Run a test query using the built DuckDB binary (which has the extension pre-loaded):

    ```bash
    # Create a simple test script
    echo "SELECT * FROM sheetreader('docker-demo/test.xlsx');" > test_verification.sql
    
    # Run it with the built binary
    ./build/release/duckdb < test_verification.sql
    ```

4.  **Confirm Success**
    - Verify that the output shows the contents of the Excel file (Numeric0 column with values).
    - Confirm that the extension builds and runs correctly with DuckDB v1.4.0+ API.
