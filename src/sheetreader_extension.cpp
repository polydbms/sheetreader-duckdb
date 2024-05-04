#include "duckdb/common/multi_file_reader.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/function.hpp"
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

SheetreaderScanData::SheetreaderScanData() {
}

SheetreaderScanData::SheetreaderScanData(ClientContext &context, vector<string> file_names, string sheet_name)
    : file_names(std::move(file_names)), sheet_name(std::move(sheet_name)) {
	// InitializeReaders(context);
	// InitializeFormats();
}

SheetreaderScanGlobalState::SheetreaderScanGlobalState(ClientContext &context, const SheetreaderScanData &bind_data)
    : bind_data(bind_data) {
}

SheetreaderScanLocalState::SheetreaderScanLocalState(ClientContext &context, SheetreaderScanGlobalState &gstate)
    : scan_count(0) {
}

SheetreaderGlobalTableFunctionState::SheetreaderGlobalTableFunctionState(ClientContext &context,
                                                                         TableFunctionInitInput &input)
    : state(context, input.bind_data->Cast<SheetreaderScanData>()) {
}

unique_ptr<GlobalTableFunctionState> SheetreaderGlobalTableFunctionState::Init(ClientContext &context,
                                                                               TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<SheetreaderScanData>();
	auto result = make_uniq<SheetreaderGlobalTableFunctionState>(context, input);
	auto &gstate = result->state;

	return std::move(result);
}

SheetreaderLocalTableFunctionState::SheetreaderLocalTableFunctionState(ClientContext &context,
                                                                       SheetreaderScanGlobalState &gstate)
    : state(context, gstate) {
}

unique_ptr<LocalTableFunctionState> SheetreaderLocalTableFunctionState::Init(ExecutionContext &context,
                                                                             TableFunctionInitInput &input,
                                                                             GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<SheetreaderGlobalTableFunctionState>();
	auto result = make_uniq<SheetreaderLocalTableFunctionState>(context.client, gstate.state);

	return std::move(result);
}

inline void SheetreaderTableFun(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {

	// Get the bind dataclass TARGET
	const SheetreaderScanData &bind_data = data_p.bind_data->Cast<SheetreaderScanData>();
	auto &gstate = data_p.global_state->Cast<SheetreaderGlobalTableFunctionState>().state;
	auto &lstate = data_p.local_state->Cast<SheetreaderLocalTableFunctionState>().state;
	// Create a single chunk with a single string column
	if (gstate.read_count > 0) {
		output.SetCardinality(0);
		return;
	}

	Vector &column = output.data[0];
	Value filename = Value("Hello World, here is your sheet name: " + bind_data.file_names[0]);
	Value sheetname = Value("Hello World, here is your sheet name: " + bind_data.sheet_name);
	column.SetValue(0, filename);
	column.SetValue(1, sheetname);
	output.SetCardinality(2);

	gstate.read_count = 1;
	lstate.scan_count = 1;
}

inline unique_ptr<FunctionData> SheetreaderBindFun(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {

	auto bind_data = make_uniq<SheetreaderScanData>();
	// TODO: Do we need any information from info?
	// TableFunctionInfo *info = input.info.get();

	// Get the file names from the first parameter
	// Note: GetFileList also checks if the files exist
	bind_data->file_names = MultiFileReader::GetFileList(context, input.inputs[0], ".XLSX (Excel)");

	// Here we could handle any named parameters
	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		if (loption == "sheetname") {
			bind_data->sheet_name = StringValue::Get(kv.second);
		}
	}

	return_types = {LogicalType::VARCHAR};
	names = {"Hello World column"};

	return bind_data;
}

static void LoadInternal(DatabaseInstance &instance) {
	// Register a table function
	TableFunction sheetreader_table_function("sheetreader", {LogicalType::VARCHAR}, SheetreaderTableFun,
	                                         SheetreaderBindFun, SheetreaderGlobalTableFunctionState::Init,
	                                         SheetreaderLocalTableFunctionState::Init);

	sheetreader_table_function.named_parameters["sheetname"] = LogicalType::VARCHAR;

	ExtensionUtil::RegisterFunction(instance, sheetreader_table_function);

	// // Register a scalar function
	// auto sheetreader_scalar_function = ScalarFunction("sheetreader", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	// SheetreaderScalarFun); ExtensionUtil::RegisterFunction(instance, sheetreader_scalar_function);

	// // Register another scalar function
	// auto sheetreader_openssl_version_scalar_function = ScalarFunction("sheetreader_openssl_version",
	// {LogicalType::VARCHAR},
	//                                             LogicalType::VARCHAR, SheetreaderOpenSSLVersionScalarFun);
	// ExtensionUtil::RegisterFunction(instance, sheetreader_openssl_version_scalar_function);
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
