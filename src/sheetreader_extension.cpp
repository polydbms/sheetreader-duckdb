#include "duckdb.h"
#include "duckdb/common/multi_file_reader.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/function.hpp"
#include "sheetreader/XlsxFile.h"

#include <string>
#define DUCKDB_EXTENSION_MAIN

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include "sheetreader_extension.hpp"

#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

// TODO: Fix default constructor
SRScanData::SRScanData() : xlsx_file("") {
}

SRScanData::SRScanData(ClientContext &context, vector<string> file_names, string sheet_name)
    : file_names(std::move(file_names)), sheet_name(std::move(sheet_name)), xlsx_file(file_names[0]) {
	// InitializeReaders(context);
	// InitializeFormats();
}

SRScanGlobalState::SRScanGlobalState(ClientContext &context, const SRScanData &bind_data)
    : bind_data(bind_data), chunk_count(0) {
}

SRScanLocalState::SRScanLocalState(ClientContext &context, SRScanGlobalState &gstate)
    : scan_count(0), bind_data(gstate.bind_data) {
}

idx_t SRScanLocalState::ReadNextBatch(SRScanGlobalState &gstate) {
	// TODO: Reset allocated memory from last batch

	// Reset scan_count
	scan_count = 0;

	// TODO: Get next batch of data from SheetReader
	GetNextBatchFromSR(gstate);

	return scan_count;
}

void SRScanLocalState::GetNextBatchFromSR(SRScanGlobalState &gstate) {
	if (gstate.chunk_count > 1) {
		scan_count = 0;
	}
	scan_count = 2048;
}

SRGlobalTableFunctionState::SRGlobalTableFunctionState(ClientContext &context, TableFunctionInitInput &input)
    : state(context, input.bind_data->Cast<SRScanData>()) {
}

unique_ptr<GlobalTableFunctionState> SRGlobalTableFunctionState::Init(ClientContext &context,
                                                                      TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<SRScanData>();
	auto result = make_uniq<SRGlobalTableFunctionState>(context, input);
	auto &gstate = result->state;

	return std::move(result);
}

SRLocalTableFunctionState::SRLocalTableFunctionState(ClientContext &context, SRScanGlobalState &gstate)
    : state(context, gstate) {
}

unique_ptr<LocalTableFunctionState> SRLocalTableFunctionState::Init(ExecutionContext &context,
                                                                    TableFunctionInitInput &input,
                                                                    GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<SRGlobalTableFunctionState>();
	auto result = make_uniq<SRLocalTableFunctionState>(context.client, gstate.state);

	return std::move(result);
}

void FillChunk(DataChunk &output, idx_t scan_count) {
	// TODO: Do speed test -- mayb SetValue is faster here?

	// Fill the output chunk with data
	// For now, we just fill it with dummy data
	for (idx_t col = 0; col < output.ColumnCount(); col++) {
		Vector &vec = output.data[col];
		auto data = FlatVector::GetData<string_t>(vec);
		// auto &validity = FlatVector::Validity(vec);
		for (idx_t i = 0; i < scan_count; i++) {
			string_t &tmp = data[i];
			tmp = "Row: " + std::to_string(i);
		}
	}
}

inline void SheetreaderTableFun(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {

	// Get the bind dataclass TARGET
	const SRScanData &bind_data = data_p.bind_data->Cast<SRScanData>();
	auto &gstate = data_p.global_state->Cast<SRGlobalTableFunctionState>().state;
	auto &lstate = data_p.local_state->Cast<SRLocalTableFunctionState>().state;

	const idx_t column_count = output.ColumnCount();

	XlsxFile file(bind_data.file_names[0]);

  // TODO: Fix this
	idx_t iterations = bind_data.iterations ? bind_data.iterations : 1;
	if (gstate.chunk_count < iterations) {
		FillChunk(output, 2048);
		output.SetCardinality(2048);
		gstate.chunk_count++;
		return;
	} else {
		output.SetCardinality(0);
		return;
	}

	Vector &column = output.data[0];
	Value filename = Value("Hello World, here is your sheet name: " + bind_data.file_names[0]);
	Value sheetname = Value("Hello World, here is your sheet name: " + bind_data.sheet_name);
	column.SetValue(0, filename);
	column.SetValue(1, sheetname);

	Vector &column2 = output.data[1];
	column2.SetValue(0, Value("Row 1"));
	column2.SetValue(1, Value("Row 2"));
	output.SetCardinality(2);

	gstate.chunk_count = 1;
	lstate.scan_count = 1;
}

inline unique_ptr<FunctionData> SheetreaderBindFun(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {

	auto bind_data = make_uniq<SRScanData>();
	// TODO: Do we need any information from info?
	// TableFunctionInfo *info = input.info.get();

	// Get the file names from the first parameter
	// Note: GetFileList also checks if the files exist
	bind_data->file_names = MultiFileReader::GetFileList(context, input.inputs[0], ".XLSX (Excel)");

	if (bind_data->file_names.size() == 0) {
		throw BinderException("No files found in path");
	} else if (bind_data->file_names.size() > 1) {
		// TODO: Support multiple files
		throw BinderException("Only one file can be read at a time");
	}

	// Here we could handle any named parameters
	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		if (loption == "sheetname") {
			bind_data->sheet_name = StringValue::Get(kv.second);
		} else if (loption == "i") {
			bind_data->iterations = IntegerValue::Get(kv.second);
		}
	}

	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	names = {"Hello World column", "Second column"};

	return std::move(bind_data);
}

static void LoadInternal(DatabaseInstance &instance) {
	// Register a table function
	TableFunction sheetreader_table_function("sheetreader", {LogicalType::VARCHAR}, SheetreaderTableFun,
	                                         SheetreaderBindFun, SRGlobalTableFunctionState::Init,
	                                         SRLocalTableFunctionState::Init);

	sheetreader_table_function.named_parameters["sheetname"] = LogicalType::VARCHAR;
	sheetreader_table_function.named_parameters["i"] = LogicalType::INTEGER;

	ExtensionUtil::RegisterFunction(instance, sheetreader_table_function);
}

void SheetreaderExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
}
std::string SheetreaderExtension::Name() {
	return "sheetreader";
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void sheetreader_init(duckdb::DatabaseInstance &db) {
	duckdb::DuckDB db_wrapper(db);
	db_wrapper.LoadExtension<duckdb::SheetreaderExtension>();
}

DUCKDB_EXTENSION_API const char *sheetreader_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
