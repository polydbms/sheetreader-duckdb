#pragma once

#include "duckdb.h"
#include "duckdb.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/function.hpp"
#include "sheetreader-core/src/XlsxFile.h"
#include "sheetreader-core/src/XlsxSheet.h"

#include <chrono>

namespace duckdb {

class SheetreaderExtension : public Extension {
public:
	void Load(DuckDB &db) override;
	std::string Name() override;
};

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

	// void Bind(ClientContext &context, TableFunctionBindInput &input);

	// void InitializeReaders(ClientContext &context);
	// void InitializeFormats();
	// void InitializeFormats(bool auto_detect);
	// void SetCompression(const string &compression);

	// static unique_ptr<SheetreaderScanData> Deserialize(Deserializer &deserializer);

public:
	//! The paths of the files we're reading
	vector<string> file_names;

	//! For testing purposes
	idx_t version = 3;

	//! All column names (in order)
	vector<string> names;

	//! All column DuckDB types (in order)
	vector<LogicalType> types;

	//! All column SheetReader types (in order)
	vector<CellType> SR_types;

	XlsxFile xlsx_file;
	unique_ptr<XlsxSheet> xlsx_sheet;

	idx_t number_threads = 1;

	idx_t skip_rows = 0;

	idx_t flag = 0;

	std::chrono::time_point<std::chrono::system_clock> start_time_parsing;
	std::chrono::time_point<std::chrono::system_clock> finish_time_parsing;

private:
	SRBindData(ClientContext &context, vector<string> file_names, string sheet_name);
};
struct SRScanGlobalState {
public:
	SRScanGlobalState(ClientContext &context, const SRBindData &bind_data);

public:

	//! Bound data
	const SRBindData &bind_data;

	//! Number of reads so far
	idx_t chunk_count = 0;

	std::chrono::time_point<std::chrono::system_clock> start_time_copy;
	std::chrono::time_point<std::chrono::system_clock> finish_time_copy;
	vector<double> times_copy = {};

	//! State of copying from mCells
	size_t max_buffers;
	size_t current_buffer;
	size_t current_thread;
	size_t current_cell;
	unsigned long current_column;
	long long current_row;
	std::vector<size_t> current_locs;
};

struct SRScanLocalState {
public:
	SRScanLocalState(ClientContext &context, SRScanGlobalState &gstate);

private:
	const SRBindData &bind_data;
};

struct SRGlobalTableFunctionState : public GlobalTableFunctionState {
public:
	SRGlobalTableFunctionState(ClientContext &context, TableFunctionInitInput &input);
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input);

public:
	SRScanGlobalState state;
};

struct SRLocalTableFunctionState : public LocalTableFunctionState {
public:
	SRLocalTableFunctionState(ClientContext &context, SRScanGlobalState &gstate);
	static unique_ptr<LocalTableFunctionState> Init(ExecutionContext &context, TableFunctionInitInput &input,
	                                                GlobalTableFunctionState *global_state);

public:
	SRScanLocalState state;
};
} // namespace duckdb
