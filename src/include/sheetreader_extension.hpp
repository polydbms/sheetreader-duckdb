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

#include <chrono>

namespace duckdb {

class SheetreaderExtension : public Extension {
public:
	void Load(DuckDB &db) override;
	std::string Name() override;
};

//! We call this ScanData analog to the JSONScanData -- BindData would be a better name
// TODO: Or should this renamed to SRReadData as in ReadCSVData?
struct SRScanData : public TableFunctionData {
public:
	//! File name with path to file
	//! Sheet ID default is 1
	SRScanData(string file_name);
	//! File name with path to file and name of sheet
	//! Throws exception if sheet name is not found
	SRScanData(string file_name, string sheet_name);
	//! File name with path to file and index of sheet (starts with 1)
	//! Throws exception if sheet at index is not found
	SRScanData(string file_name, int sheet_index);

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

	XlsxFile xlsx_file;
	unique_ptr<XlsxSheet> xlsx_sheet;

	// TODO: Which default value should be used?
	idx_t number_threads = 1;

	// TODO: Which default value should be used?
	idx_t skip_rows = 0;

	idx_t flag = 0;

	//! Coerce all cells to string in user defined column types
	bool coerce_to_string = false;

	//! User defined types
	vector<LogicalType> user_types = {};

	std::chrono::time_point<std::chrono::system_clock> start_time_parsing;
	std::chrono::time_point<std::chrono::system_clock> finish_time_parsing;

private:
	SRScanData(ClientContext &context, vector<string> file_names, string sheet_name);
};
struct SRScanGlobalState {
public:
	SRScanGlobalState(ClientContext &context, const SRScanData &bind_data);

public:

	//! Bound data
	const SRScanData &bind_data;

	//! Number of reads so far
	idx_t chunk_count = 0;

	std::chrono::time_point<std::chrono::system_clock> start_time_copy;
	std::chrono::time_point<std::chrono::system_clock> finish_time_copy;
	vector<double> times_copy = {};

	//! State of copying from mCells
	size_t maxBuffers;
	size_t currentBuffer;
	size_t currentThread;
	size_t currentCell;
	unsigned long currentColumn;
	long long currentRow;
	std::vector<size_t> currentLocs;
};

struct SRScanLocalState {
public:
	SRScanLocalState(ClientContext &context, SRScanGlobalState &gstate);

public:
	//! Get next batch of data and return number of rows gathered
	idx_t ReadNextBatch(SRScanGlobalState &gstate);
	void GetNextBatchFromSR(SRScanGlobalState &gstate);

public:
	//! Current scan data
	idx_t scan_count;

private:
	const SRScanData &bind_data;
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
