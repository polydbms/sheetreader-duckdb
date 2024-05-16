#include "duckdb.h"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/multi_file_reader.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/function.hpp"
#include "sheetreader/XlsxFile.h"
#include "sheetreader/XlsxSheet.h"

#include <cmath>
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
SRScanData::SRScanData(string file_name)
		: xlsx_file(file_name), xlsx_sheet(make_uniq<XlsxSheet>(xlsx_file.getSheet(1))) {
	//, xlsx_file(make_uniq<XlsxFile>(file_name))
}

SRScanData::SRScanData(ClientContext &context, vector<string> file_names, string sheet_name)
    : file_names(std::move(file_names)), sheet_name(std::move(sheet_name)), xlsx_file(file_names[0]), xlsx_sheet(make_uniq<XlsxSheet>(xlsx_file.getSheet(1))) {
			//, xlsx_file(make_uniq<XlsxFile>(file_names[0]))
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

	auto &xlsx_file = bind_data.xlsx_file;
	auto &fsheet = bind_data.xlsx_sheet;

	const idx_t column_count = output.ColumnCount();


	// Get the next batch of data from sheetreader
	idx_t i = 0;
	for(; i < 2048; i++) {
		// TODO: fsheet is const due to bind_data being const. Maybe we should store the sheet in the local/global state?
		auto row = fsheet->nextRow();
		if (row.first == 0) {
			break;
		}
		auto &row_values = row.second;

		for(idx_t j = 0; j < column_count; j++) {
			switch (bind_data.types[j].id()) {
				case LogicalTypeId::VARCHAR: {
					// TODO: Check if types align
					auto value = xlsx_file.getString(row_values[j].data.integer) ;
					output.data[j].SetValue(i, Value(value));
					break;
				}
				case LogicalTypeId::DOUBLE: {
					auto value = row_values[j].data.real;
					output.data[j].SetValue(i, Value(value));
					break;
				}
				case LogicalTypeId::BOOLEAN: {
					auto value = row_values[j].data.boolean;
					output.data[j].SetValue(i, Value(value));
					break;
				}
				case LogicalTypeId::DATE: {
					date_t value = date_t((int)(row_values[j].data.real / 86400.0));
					output.data[j].SetValue(i, Value::DATE(value));
					break;
				}
				default:
					throw InternalException("This shouldn't happen. Unsupported Logical type");
			
			}
			

		}
	}
	output.SetCardinality(i);

	gstate.chunk_count = 1;
	lstate.scan_count = 1;
}

inline unique_ptr<FunctionData> SheetreaderBindFun(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {

	// Get the file names from the first parameter
	// Note: GetFileList also checks if the files exist
	auto file_names = MultiFileReader::GetFileList(context, input.inputs[0], ".XLSX (Excel)");

	if (file_names.size() == 0) {
		throw BinderException("No files found in path");
	} else if (file_names.size() > 1) {
		// TODO: Support multiple files
		throw BinderException("Only one file can be read at a time");
	}

	auto bind_data = make_uniq<SRScanData>(file_names[0]);


	// TODO: Do we need any information from info?
	// TableFunctionInfo *info = input.info.get();

	// Here we could handle any named parameters
	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		if (loption == "sheetname") {
			bind_data->sheet_name = StringValue::Get(kv.second);
		} else if (loption == "i") {
			bind_data->iterations = IntegerValue::Get(kv.second);
		} else if (loption == "threads") {
			bind_data->number_threads = IntegerValue::Get(kv.second);
		} else {
			throw BinderException("Unknown named parameter");
		}
	}


	// If number threads > 1, we set parallel true
	if (bind_data->number_threads > 1) {
		bind_data->xlsx_file.mParallelStrings = true;
	} else {
		bind_data->xlsx_file.mParallelStrings = false;
	}

	bind_data->xlsx_file.parseSharedStrings();



	// TODO: Make sheet number / name configurable via named parameters
	auto &fsheet = bind_data->xlsx_sheet;

	// TODO: Make this configurable (especially skipRows) + number_threads via named parameters
	bool success = fsheet->interleaved(1, 0, bind_data->number_threads);
	if(!success) {
		throw BinderException("Failed to read sheet");
	}
	bind_data->xlsx_file.finalize();

	auto number_column = fsheet->mDimension.first;
	auto number_rows = fsheet->mDimension.second;

	vector<CellType> colTypesByIndex;
	// Get types of columns
	for(idx_t i = 0; i < number_column; i++) {
		colTypesByIndex.push_back(fsheet->mCells[0].front()[i].type);
	}

	// Convert CellType to LogicalType
	vector<LogicalType> column_types;
	vector<string> column_names;
	idx_t column_index = 0;
	for(auto &colType : colTypesByIndex) {
		switch(colType) {
			case CellType::T_STRING_REF:
				column_types.push_back(LogicalType::VARCHAR);
				column_names.push_back("String" + std::to_string(column_index));
				break;
			case CellType::T_STRING:
			case CellType::T_STRING_INLINE:
				// TODO
				throw BinderException("Inline & dynamic String types not supported yet");
				break;
			case CellType::T_NUMERIC:
				column_types.push_back(LogicalType::DOUBLE);
				column_names.push_back("Numeric" + std::to_string(column_index));
				break;
			case CellType::T_BOOLEAN:
				column_types.push_back(LogicalType::BOOLEAN);
				column_names.push_back("Boolean" + std::to_string(column_index));
				break;
			case CellType::T_DATE:
				// TODO: Fix date type
				column_types.push_back(LogicalType::DATE);
				column_names.push_back("Date"+ std::to_string(column_index));
				break;
			default:
				// TODO: Add specific column or data type
				throw BinderException("Unknown cell type in column ");
		}
		column_index++;
	}

	return_types = column_types;
	bind_data->types = column_types;
	// TODO: Extract column names from sheet or named parameters or use default names
	names = column_names;
	bind_data->names = column_names;


	// First row is discarded
	// TODO: Remove when not using nextRow() anymore
	auto discarded_val = fsheet->nextRow();


	return std::move(bind_data);
}

static void LoadInternal(DatabaseInstance &instance) {
	// Register a table function
	TableFunction sheetreader_table_function("sheetreader", {LogicalType::VARCHAR}, SheetreaderTableFun,
	                                         SheetreaderBindFun, SRGlobalTableFunctionState::Init,
	                                         SRLocalTableFunctionState::Init);

	sheetreader_table_function.named_parameters["sheetname"] = LogicalType::VARCHAR;
	sheetreader_table_function.named_parameters["i"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["threads"] = LogicalType::INTEGER;

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
