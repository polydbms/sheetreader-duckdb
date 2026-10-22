# Extension updating

This repository keeps a branch per DuckDB minor line (e.g. `1.4`, `1.5`, `2.0`), matching how DuckDB itself maintains
`vx.y-codename` release branches. `main` tracks the newest line; older lines stay on their version branch for
maintenance builds.

## After a DuckDB patch/minor release lands

1. Create or check out the version branch for that line, e.g. `2.0` for DuckDB 2.0.x.
2. Bump submodules on that branch:
   - `./duckdb` → latest tag on that line (e.g. `v2.0.0`)
   - `./extension-ci-tools` → matching branch/tag (e.g. `v2.0-cyanoptera`)
3. Bump `.github/workflows/MainDistributionPipeline.yml`:
   - `duckdb_version` → that tag
   - reusable workflow ref / `ci_tools_version` → matching CI tools branch
4. Merge/push the version branch, then open a PR in
   [duckdb/community-extensions](https://github.com/duckdb/community-extensions) updating
   `extensions/sheetreader/description.yml` `repo.ref` to the new commit SHA so community binaries are
   rebuilt. The website list is generated from binaries available for the current DuckDB version — if the
   rebuild is missing, the extension disappears from the list even though the descriptor still exists.

## Before an upcoming DuckDB minor (feature freeze)

Follow the community-extensions `ref_next` flow: develop on `vx.y-codename` (or your `x.y` branch), point
`repo.ref_next` at that commit so binaries are ready on release day.
