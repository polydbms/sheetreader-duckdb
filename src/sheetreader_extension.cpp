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

SheetreaderScanData::SheetreaderScanData(ClientContext &context, vector<string> files_p)
    : files(std::move(files_p)) {
	// InitializeReaders(context);
	// InitializeFormats();
}


inline void SheetreaderTableFun(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	// Create a single chunk with a single string column
	Vector &column = output.data[0];
	Value val = Value("Hello World");
	column.SetValue(0, val);
	output.SetCardinality(1);
}

inline unique_ptr<FunctionData> SheetreaderBindFun(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {

	auto bind_data = make_uniq<SheetreaderScanData>();
	// TODO: Do we need any information from info?
	// TableFunctionInfo *info = input.info.get();

	// Get the file names from the first parameter
	vector<string> file_names = MultiFileReader::GetFileList(context, input.inputs[0], ".XLSX (Excel)");

	// Here we could handle any named parameters
	// for (auto &kv : input.named_parameters) {
	// }

	// Create a TableFunctionData object and return it
	TableFunctionData tableFunctionData;
	// tableFunctionData.Initialize(file_names);

	return_types = {LogicalType::VARCHAR};
	names = {"Hello World column"};

	return make_uniq<TableFunctionData>(tableFunctionData);
}

static void LoadInternal(DatabaseInstance &instance) {
	// Register a table function
	TableFunction sheetreader_table_function =
	    TableFunction("sheetreader", {LogicalType::VARCHAR}, SheetreaderTableFun, SheetreaderBindFun);
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
