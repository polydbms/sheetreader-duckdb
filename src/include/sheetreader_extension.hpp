#pragma once

#include "duckdb.h"
#include "duckdb.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/function.hpp"
#include "sheetreader-core/src/XlsxFile.h"
#include "sheetreader-core/src/XlsxSheet.h"

namespace duckdb {

class SheetreaderExtension : public Extension {
public:
	void Load(DuckDB &db) override;
	std::string Name() override;
	std::string Version() const override;
};

//! Contains all data that is determined during the bind function
struct SRBindData : public TableFunctionData {
public:
	//! File name with path to file
	//! Sheet ID default is 1
	SRBindData(string file_name);
	//! File name with path to file and name of sheet
	//! Throws exception if sheet name is not found
	SRBindData(string file_name, string sheet_name);
	//! File name with path to file and index of sheet (starts with 1)
	//! Throws exception if sheet at index is not found
	SRBindData(string file_name, int sheet_index);

public:
	//! The paths of the files we're reading
	vector<string> file_names;

	//! All column names (in order)
	vector<string> names;

	//! All column DuckDB types (in order)
	vector<LogicalType> types;

	//! The .XLSX-file -- created by sheetreader-core
	XlsxFile xlsx_file;
	//! A sheet of xlsx_file -- created by sheetreader-core
	unique_ptr<XlsxSheet> xlsx_sheet;

	//! Number of threads used while parsing
	idx_t number_threads = 1;

	//! Number of rows to skip while parsing
	idx_t skip_rows = 0;

	//! Coerce all cells to string in user defined column types
	bool coerce_to_string = false;

	//! User defined types
	vector<LogicalType> user_types = {};

	//! Use user_types even if they are not compatible with types determined by first/second row
	bool force_types = false;

private:
	SRBindData(ClientContext &context, vector<string> file_names, string sheet_name);
};
//! Keeps state in between calls to the table (copy) function
struct SRGlobalState {
public:
	SRGlobalState(ClientContext &context, const SRBindData &bind_data);

public:
	//! Bound data
	const SRBindData &bind_data;

	//! Number of chunk read so far
	idx_t chunk_count = 0;

	//! State of copying from mCells
	size_t max_buffers;
	//! Current index of thread
	size_t current_thread;
	//! Current index of buffer in thread
	size_t current_buffer;
	//! Current index of cell in buffer
	size_t current_cell;
	//! Current index of column in row
	unsigned long current_column;
	//! Current index of row in sheet
	long long current_row;
	//! Current index of row per thread
	std::vector<size_t> current_locs;
};

struct SRLocalState {
public:
	SRLocalState(ClientContext &context, SRGlobalState &gstate);

private:
	const SRBindData &bind_data;
};

//! Contains SRGlobalState
struct SRGlobalTableFunctionState : public GlobalTableFunctionState {
public:
	SRGlobalTableFunctionState(ClientContext &context, TableFunctionInitInput &input);
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input);

public:
	SRGlobalState state;
};

struct SRLocalTableFunctionState : public LocalTableFunctionState {
public:
	SRLocalTableFunctionState(ClientContext &context, SRGlobalState &gstate);
	static unique_ptr<LocalTableFunctionState> Init(ExecutionContext &context, TableFunctionInitInput &input,
	                                                GlobalTableFunctionState *global_state);

public:
	SRLocalState state;
};
} // namespace duckdb
