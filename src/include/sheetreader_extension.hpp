#pragma once

#include "duckdb.h"
#include "duckdb.hpp"
#include "duckdb/function/function.hpp"

namespace duckdb {

class SheetreaderExtension : public Extension {
public:
	void Load(DuckDB &db) override;
	std::string Name() override;
};

struct SheetreaderScanData : public TableFunctionData {
public:
	SheetreaderScanData();

	// void Bind(ClientContext &context, TableFunctionBindInput &input);

	// void InitializeReaders(ClientContext &context);
	// void InitializeFormats();
	// void InitializeFormats(bool auto_detect);
	// void SetCompression(const string &compression);

	// static unique_ptr<SheetreaderScanData> Deserialize(Deserializer &deserializer);

public:
	//! The files we're reading
	vector<string> file_names;

	//! Name of the sheet to read
	string sheet_name;
	

	// //! Whether or not we should ignore malformed JSON (default to NULL)
	// bool ignore_errors = false;
	// //! Maximum JSON object size (defaults to 16MB minimum)
	// idx_t maximum_object_size = 16777216;
	// //! Whether we auto-detect a schema
	// bool auto_detect = false;
	// //! Sample size for detecting schema
	// idx_t sample_size = idx_t(STANDARD_VECTOR_SIZE) * 10;
	// //! Max depth we go to detect nested JSON schema (defaults to unlimited)
	// idx_t max_depth = NumericLimits<idx_t>::Maximum();
	// //! We divide the number of appearances of each JSON field by the auto-detection sample size
	// //! If the average over the fields of an object is less than this threshold,
	// //! we default to the JSON type for this object rather than the shredded type
	// double field_appearance_threshold = 0.1;
	// //! The maximum number of files we sample to sample sample_size rows
	// idx_t maximum_sample_files = 32;
	// //! Whether we auto-detect and convert JSON strings to integers
	// bool convert_strings_to_integers = false;

	//! All column names (in order)
	vector<string> names;

	//! The inferred avg tuple size
	idx_t avg_tuple_size = 420;
private:
	SheetreaderScanData(ClientContext &context, vector<string> file_names, string sheet_name);

};
struct SheetreaderScanGlobalState {
public:
	SheetreaderScanGlobalState(ClientContext &context, const SheetreaderScanData &bind_data);

public:
	//! Bound data
	const SheetreaderScanData &bind_data;

	//! Number of reads so far
	idx_t read_count = 0;
};

struct SheetreaderScanLocalState {
public:
	SheetreaderScanLocalState(ClientContext &context, SheetreaderScanGlobalState &gstate);

// public:
// 	idx_t ReadNext(JSONScanGlobalState &gstate);

public:
	//! Current scan data
	idx_t scan_count;
};

struct SheetreaderGlobalTableFunctionState : public GlobalTableFunctionState {
public:
	SheetreaderGlobalTableFunctionState(ClientContext &context, TableFunctionInitInput &input);
	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input);

public:
	SheetreaderScanGlobalState state;
};

struct SheetreaderLocalTableFunctionState : public LocalTableFunctionState {
public:
	SheetreaderLocalTableFunctionState(ClientContext &context, SheetreaderScanGlobalState &gstate);
	static unique_ptr<LocalTableFunctionState> Init(ExecutionContext &context, TableFunctionInitInput &input,
	                                                GlobalTableFunctionState *global_state);

public:
	SheetreaderScanLocalState state;
};
} // namespace duckdb
