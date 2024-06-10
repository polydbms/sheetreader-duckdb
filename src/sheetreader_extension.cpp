#include "duckdb.h"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/multi_file_reader.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/function.hpp"
#include "sheetreader-core/src/XlsxFile.h"
#include "sheetreader-core/src/XlsxSheet.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
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

inline idx_t DefaultThreads() {
	idx_t sys_number_threads = std::thread::hardware_concurrency();

	idx_t appropriate_number_threads = sys_number_threads / 2;

	if (appropriate_number_threads <= 0) {
		appropriate_number_threads = 1;
	}

	return appropriate_number_threads;

}

SRBindData::SRBindData(string file_name)
    : SRBindData(file_name,1) {
}

SRBindData::SRBindData(string file_name, string sheet_name)
    : xlsx_file(file_name), xlsx_sheet(make_uniq<XlsxSheet>(xlsx_file.getSheet(sheet_name))), number_threads(DefaultThreads()) {
}

SRBindData::SRBindData(string file_name, int sheet_index)
    : xlsx_file(file_name), xlsx_sheet(make_uniq<XlsxSheet>(xlsx_file.getSheet(sheet_index))), number_threads(DefaultThreads()) {
}

SRScanGlobalState::SRScanGlobalState(ClientContext &context, const SRBindData &bind_data)
    : bind_data(bind_data), chunk_count(0) {
}

SRScanLocalState::SRScanLocalState(ClientContext &context, SRScanGlobalState &gstate) : bind_data(gstate.bind_data) {
}

SRGlobalTableFunctionState::SRGlobalTableFunctionState(ClientContext &context, TableFunctionInitInput &input)
    : state(context, input.bind_data->Cast<SRBindData>()) {
}

unique_ptr<GlobalTableFunctionState> SRGlobalTableFunctionState::Init(ClientContext &context,
                                                                      TableFunctionInitInput &input) {

	auto result = make_uniq<SRGlobalTableFunctionState>(context, input);

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

inline void FinishChunk(DataChunk &output, idx_t cardinality, SRScanGlobalState &gstate,
                        std::chrono::time_point<std::chrono::system_clock> start_time_copy_chunk,
                        bool print_time = false) {

	// Indicate how many rows are in the chunk
	// If cardinality is 0, it means that the chunk is empty and no more rows are to be expected
	output.SetCardinality(cardinality);

	// For benchmarking purposes, we store the time it took to copy the chunk
	std::chrono::time_point<std::chrono::system_clock> finish_time_copy_chunk = std::chrono::system_clock::now();
	std::chrono::duration<double> elapsed_seconds_chunk = finish_time_copy_chunk - start_time_copy_chunk;
	gstate.times_copy.push_back(elapsed_seconds_chunk.count());

	if (cardinality == 0 && print_time) {
		gstate.finish_time_copy = std::chrono::system_clock::now();
		std::chrono::duration<double> elapsed_seconds = gstate.finish_time_copy - gstate.start_time_copy;
		std::cout << "Copy time: " << elapsed_seconds.count() << "s" << std::endl;

		double sum = 0;
		for (auto &time : gstate.times_copy) {
			sum += time;
		}
		std::cout << "Pure Copy time: " << sum << "s" << std::endl;
	}

	gstate.chunk_count++;
	return;
}

union DataPtr {
	string_t *string_data;
	double *double_data;
	bool *bool_data;
	date_t *date_data;
};

inline void SetNull(const SRBindData &bind_data, DataChunk &output, vector<DataPtr> &flat_vectors, const XlsxCell &cell,
                    idx_t row_id, idx_t column_id) {
	// Set cell to NULL
	LogicalType expected_type = bind_data.types[column_id];
	output.data[column_id].SetValue(row_id, Value(expected_type));
}

inline void SetAllInvalid(DataChunk &output, idx_t cardinality) {
	for (idx_t col = 0; col < output.ColumnCount(); col++) {
		Vector &vec = output.data[col];
		auto &validity = FlatVector::Validity(vec);
		validity.SetAllInvalid(cardinality);
	}
}

inline void SetCell(const SRBindData &bind_data, DataChunk &output, vector<DataPtr> &flat_vectors, const XlsxCell &cell,
                    idx_t row_id, idx_t column_id) {

	auto &xlsx_file = bind_data.xlsx_file;

	Vector &vec = output.data[column_id];
	auto &validity = FlatVector::Validity(vec);
	validity.SetValid(row_id);

	switch (bind_data.types[column_id].id()) {
	case LogicalTypeId::VARCHAR: {
		auto value = xlsx_file.getString(cell.data.integer);
		// string_t creates values that fail the UTF-8 check, so we use the unperformant technique
		// flat_vectors[j].string_data[i] = string_t(value);
		output.data[column_id].SetValue(row_id, Value(value));
		break;
	}
	case LogicalTypeId::DOUBLE: {
		auto value = cell.data.real;
		flat_vectors[column_id].double_data[row_id] = value;
		break;
	}
	case LogicalTypeId::BOOLEAN: {
		auto value = cell.data.boolean;
		flat_vectors[column_id].bool_data[row_id] = value;
		break;
	}
	case LogicalTypeId::DATE: {
		date_t value = date_t((int)(cell.data.real / 86400.0));
		flat_vectors[column_id].date_data[row_id] = value;
		break;
	}
	default:
		throw InternalException("This shouldn't happen. Unsupported Logical type");
	}
}

bool CheckRowLimitReached(SRScanGlobalState &gstate) {
	long long row_offset = gstate.chunk_count * STANDARD_VECTOR_SIZE;
	long long limit = row_offset + STANDARD_VECTOR_SIZE;
	long long skipRows = gstate.bind_data.xlsx_sheet->mSkipRows;
	bool limit_reached = gstate.current_row - skipRows >= limit;
	return limit_reached;
}

idx_t GetCardinality(SRScanGlobalState &gstate) {
	long long row_offset = gstate.chunk_count * STANDARD_VECTOR_SIZE;
	long long skipRows = gstate.bind_data.xlsx_sheet->mSkipRows;
	// This is the case when no new rows are copied and last chunk was not full (last iteration)
	if (gstate.current_row + 1 < skipRows + row_offset) {
		return 0;
	}
	return gstate.current_row - skipRows - row_offset + 1;
}

//! Assume that types of XlsxCells align with the types of the columns
//! Returns Cardinality (number of rows copied)
size_t UnsafeCopy(SRScanGlobalState &gstate, const SRBindData &bind_data, DataChunk &output,
                  vector<DataPtr> &flat_vectors) {

	auto &sheet = bind_data.xlsx_sheet;

	D_ASSERT(bind_data.number_threads == sheet->mCells.size());

	idx_t number_threads = bind_data.number_threads;

	if (number_threads == 0) {
		return 0;
	}

	size_t row_offset = gstate.chunk_count * STANDARD_VECTOR_SIZE;

	auto calcAdjustedRow = [row_offset](long long currentRow, unsigned long skip_rows) {
		return currentRow - skip_rows - row_offset;
	};

	// Initialize state
	if (gstate.current_locs.size() == 0) {
		// Get number of buffers from first thread (is always the maximum)
		gstate.max_buffers = sheet->mCells[0].size();
		gstate.current_buffer = 0;
		gstate.current_thread = 0;
		gstate.current_cell = 0;
		gstate.current_column = 0;
		gstate.current_row = -1;
		gstate.current_locs = std::vector<size_t>(number_threads, 0);
	}

	// Set all values to NULL per default (sheetreader-core doesn't store information about empty cells)
	SetAllInvalid(output, STANDARD_VECTOR_SIZE);

	//! To get the correct order of rows we iterate for(buffer_index) { for(thread_index) { ... } }
	//! This is due to how sheetreader-core writes the data to the buffers (stored in mCells)
	for (; gstate.current_buffer < gstate.max_buffers; ++gstate.current_buffer) {
		for (; gstate.current_thread < sheet->mCells.size(); ++gstate.current_thread) {

			// If there are no more buffers to read, return last row and prepare for finishing copy
			if (sheet->mCells[gstate.current_thread].size() == 0) {
				// Set to maxBuffers, so this is the last iteration
				gstate.current_buffer = gstate.max_buffers;

				return GetCardinality(gstate);
			}

			//! Get current cell buffer
			const std::vector<XlsxCell> cells = sheet->mCells[gstate.current_thread].front();
			//! Location info for current thread
			const std::vector<LocationInfo> &locs_infos = sheet->mLocationInfos[gstate.current_thread];
			//! Current location index
			size_t &currentLoc = gstate.current_locs[gstate.current_thread];

			// This is a weird implementation detail of sheetreader-core:
			// currentCell <= cells.size() because there might be location info after last cell
			for (; gstate.current_cell <= cells.size(); ++gstate.current_cell) {

				// Update currentRow & currentColumn when location info is available for current cell at currentLoc
				// After setting those values: Advance to next location info
				//
				// This means that the values won't be updated if there is no location info for the current cell
				// (e.g. not first cell in row)
				//
				// Loop is executed n+1 times for first location info, where n is the number of skip_rows (specified as
				// parameter for interleaved) This is because, SheetReader creates location infos for the skipped lines
				// with cell == column == buffer == 0 sames as for the first "real" row
				while (currentLoc < locs_infos.size() && locs_infos[currentLoc].buffer == gstate.current_buffer &&
				       locs_infos[currentLoc].cell == gstate.current_cell) {

					gstate.current_column = locs_infos[currentLoc].column;
					if (locs_infos[currentLoc].row == -1ul) {
						++gstate.current_row;
					} else {
						gstate.current_row = locs_infos[currentLoc].row;
					}

					long long adjustedRow = calcAdjustedRow(gstate.current_row, sheet->mSkipRows);

					// This only happens for header rows -- we want to skip them
					if (adjustedRow < 0) {
						++currentLoc;
						// Skip to next row
						if (currentLoc < locs_infos.size()) {
							gstate.current_cell = locs_infos[currentLoc].cell;
						} else {
							throw InternalException("Skipped more rows than available in first buffer -- consider "
							                        "decreasing number of threads");
						}
						continue;
					}

					// Increment for next iteration
					++currentLoc;

					if (CheckRowLimitReached(gstate)) {
						return (GetCardinality(gstate) - 1);
					}
				}
				// We need to check this here, because we iterate to cells.size() to get location info
				if (gstate.current_cell >= cells.size())
					break;

				const auto currentColumn = gstate.current_column;

				// If this cell is in a column that was not present in the first row, we throw an error
				if (currentColumn >= bind_data.types.size()) {
					// throw InvalidInputException("Row " + std::to_string(gstate.currentRow) +
					//                             "has more columns than the first row: " +
					//                             std::to_string(currentColumn + 1));
					// currentRowData.resize(fsheet->mDimension.first - fsheet->mSkipColumns, XlsxCell());
					std::cout << "Row " << gstate.current_row
					          << " has more columns than the first row: " << currentColumn + 1 << std::endl;
				}
				const XlsxCell &cell = cells[gstate.current_cell];
				long long mSkipRows = sheet->mSkipRows;
				long long adjustedRow = calcAdjustedRow(gstate.current_row, mSkipRows);

				bool types_align = cell.type == bind_data.SR_types[currentColumn];
				// sheetreader-core doesn't determine empty cells to be T_NONE, instead it skips the cell,
				// so it's not stored in mCells. We handle this by setting all cells as Invalid (aka null)
				// and set them valid when they appear in mCells
				if (cell.type == CellType::T_NONE || cell.type == CellType::T_ERROR || !types_align) {
					SetNull(bind_data, output, flat_vectors, cell, adjustedRow, currentColumn);
				} else {
					// std::cout << "Row: " << adjustedRow << " Adjusted Column: " << currentColumn << std::endl;
					SetCell(bind_data, output, flat_vectors, cell, adjustedRow, currentColumn);
				}
				++gstate.current_column;
			}
			sheet->mCells[gstate.current_thread].pop_front();
			gstate.current_cell = 0;
		}
		gstate.current_thread = 0;
	}
	return GetCardinality(gstate);
}

inline void SheetreaderTableFun(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {

	// Get the bind dataclass TARGET
	const SRBindData &bind_data = data_p.bind_data->Cast<SRBindData>();
	auto &gstate = data_p.global_state->Cast<SRGlobalTableFunctionState>().state;
	auto &lstate = data_p.local_state->Cast<SRLocalTableFunctionState>().state;

	if (gstate.chunk_count == 0) {
		gstate.start_time_copy = std::chrono::system_clock::now();
	}

	auto start_time_copy_chunk = std::chrono::system_clock::now();

	auto &xlsx_file = bind_data.xlsx_file;
	auto &sheet = bind_data.xlsx_sheet;

	const idx_t column_count = output.ColumnCount();

	// Store FlatVectors for all columns (they have different data types)
	vector<DataPtr> flat_vectors;

	// Is this useful?
	// data_ptr_t dataptr = output.data[0].GetData();

	for (idx_t col = 0; col < column_count; col++) {
		switch (bind_data.types[col].id()) {
		case LogicalTypeId::VARCHAR: {
			Vector &vec = output.data[col];
			string_t *data_vec = FlatVector::GetData<string_t>(vec);
			DataPtr data;
			data.string_data = data_vec;
			// Store pointer to data
			flat_vectors.push_back(data);
			break;
		}
		case LogicalTypeId::DOUBLE: {
			Vector &vec = output.data[col];
			auto data_vec = FlatVector::GetData<double>(vec);
			DataPtr data;
			data.double_data = data_vec;
			flat_vectors.push_back(data);
			break;
		}
		case LogicalTypeId::BOOLEAN: {
			Vector &vec = output.data[col];
			auto data_vec = FlatVector::GetData<bool>(vec);
			DataPtr data;
			data.bool_data = data_vec;
			flat_vectors.push_back(data);
			break;
		}
		case LogicalTypeId::DATE: {
			Vector &vec = output.data[col];
			auto data_vec = FlatVector::GetData<date_t>(vec);
			DataPtr data;
			data.date_data = data_vec;
			flat_vectors.push_back(data);
			break;
		}
		default:
			throw InternalException("This shouldn't happen. Unsupported Logical type");
		}
	}

	if (bind_data.version == 0) {
		// This version uses SetValue for all types

		// Get the next batch of data from sheetreader
		idx_t i = 0;
		for (; i < STANDARD_VECTOR_SIZE; i++) {
			auto row = sheet->nextRow();
			if (row.first == 0) {
				break;
			}
			auto &row_values = row.second;

			for (idx_t j = 0; j < column_count; j++) {
				switch (bind_data.types[j].id()) {
				case LogicalTypeId::VARCHAR: {
					auto value = xlsx_file.getString(row_values[j].data.integer);
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

		FinishChunk(output, i, gstate, start_time_copy_chunk);

		return;

	} else if (bind_data.version == 1) {
		// This version uses SetValue only for VARCHAR, for other types it uses directly the flat vectors

		// Get the next batch of data from sheetreader
		idx_t i = 0;
		for (; i < STANDARD_VECTOR_SIZE; i++) {

			auto row = sheet->nextRow();
			if (row.first == 0) {
				break;
			}
			auto &row_values = row.second;

			for (idx_t j = 0; j < column_count; j++) {
				switch (bind_data.types[j].id()) {
				case LogicalTypeId::VARCHAR: {
					auto value = xlsx_file.getString(row_values[j].data.integer);
					// string_t creates values that fail the UTF-8 check, so we use the unperformant technique
					// flat_vectors[j].string_data[i] = string_t(value);
					output.data[j].SetValue(i, Value(value));
					break;
				}
				case LogicalTypeId::DOUBLE: {
					auto value = row_values[j].data.real;
					flat_vectors[j].double_data[i] = value;
					break;
				}
				case LogicalTypeId::BOOLEAN: {
					auto value = row_values[j].data.boolean;
					flat_vectors[j].bool_data[i] = value;
					break;
				}
				case LogicalTypeId::DATE: {
					date_t value = date_t((int)(row_values[j].data.real / 86400.0));
					flat_vectors[j].date_data[i] = value;
					break;
				}
				default:
					throw InternalException("This shouldn't happen. Unsupported Logical type");
				}
			}
		}

		FinishChunk(output, i, gstate, start_time_copy_chunk, bind_data.flag == 1);

		return;

	} else if (bind_data.version == 3) {
		// This version doesn't use nextRow() and has more features (coercion to string, handling empty cells, etc.)

		auto cardinality = UnsafeCopy(gstate, bind_data, output, flat_vectors);

		FinishChunk(output, cardinality, gstate, start_time_copy_chunk, bind_data.flag == 1);

		return;
	}
}

inline bool ConvertCellTypes(vector<LogicalType> &column_types, vector<string> &column_names,
                             vector<CellType> &colTypesByIndex) {
	idx_t column_index = 0;
	bool first_row_all_string = true;
	for (auto &colType : colTypesByIndex) {
		switch (colType) {
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
			first_row_all_string = false;
			break;
		case CellType::T_BOOLEAN:
			column_types.push_back(LogicalType::BOOLEAN);
			column_names.push_back("Boolean" + std::to_string(column_index));
			first_row_all_string = false;
			break;
		case CellType::T_DATE:
			column_types.push_back(LogicalType::DATE);
			column_names.push_back("Date" + std::to_string(column_index));
			first_row_all_string = false;
			break;
		default:
			throw BinderException("Unknown cell type in column in column " + std::to_string(column_index));
		}
		column_index++;
	}
	return first_row_all_string;
}

inline vector<string> GetHeaderNames(vector<XlsxCell> &row, SRBindData &bind_data) {

	vector<string> column_names;

	for (idx_t j = 0; j < row.size(); j++) {
		switch (row[j].type) {
		case CellType::T_STRING_REF: {
			auto value = bind_data.xlsx_file.getString(row[j].data.integer);
			column_names.push_back(value);
			break;
		}
		case CellType::T_STRING:
		case CellType::T_STRING_INLINE: {
			// TODO
			throw BinderException("Inline & dynamic String types not supported yet");
			break;
		}
		default:
			throw BinderException("Header row contains non-string values");
		}
	}

	return column_names;
}

inline unique_ptr<FunctionData> SheetreaderBindFun(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {

	auto file_reader = MultiFileReader::Create(input.table_function);
	auto file_list = file_reader->CreateFileList(context, input.inputs[0]);
	auto file_names = file_list->GetAllFiles();

	if (file_names.size() == 0) {
		throw BinderException("No files found in path");
	} else if (file_names.size() > 1) {
		throw BinderException("Only one file can be read at a time");
	}

	string sheet_name;
	int sheet_index;
	bool sheet_index_set = false;

	bool use_header = false;

	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		if (loption == "sheet_name") {
			sheet_name = StringValue::Get(kv.second);
		} else if (loption == "sheet_index") {
			sheet_index = IntegerValue::Get(kv.second);
			sheet_index_set = true;
		} else if (loption == "has_header") {
			use_header = BooleanValue::Get(kv.second);
		} else {
			continue;
		}
	}

	if (!sheet_name.empty() && sheet_index_set) {
		throw BinderException("Sheet index & sheet name cannot be set at the same time.");
	}

	// Create the bind data object and return it
	unique_ptr<duckdb::SRBindData> bind_data;

	try {
		if (!sheet_name.empty()) {
			bind_data = make_uniq<SRBindData>(file_names[0], sheet_name);
		} else if (sheet_index_set) {
			bind_data = make_uniq<SRBindData>(file_names[0], sheet_index);
		} else {
			bind_data = make_uniq<SRBindData>(file_names[0]);
		}
	} catch (std::exception &e) {
		throw BinderException(e.what());
	}

	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		if (loption == "version") {
			bind_data->version = IntegerValue::Get(kv.second);
		} else if (loption == "threads") {
			bind_data->number_threads = IntegerValue::Get(kv.second);
			if (bind_data->number_threads <= 0) {
				throw BinderException("Number of threads must be greater than 0");
			}
		} else if (loption == "flag") {
			bind_data->flag = IntegerValue::Get(kv.second);
		} else if (loption == "skip_rows") {
			// Default: 0
			bind_data->skip_rows = IntegerValue::Get(kv.second);
			if (bind_data->skip_rows < 0) {
				throw BinderException("Number of rows to skip must be greater than or equal to 0");
			}
		} else if (loption == "sheet_name" || loption == "sheet_index" || loption == "has_header") {
			continue;
		} else {
			throw BinderException("Unknown named parameter");
		}
	}

	// Doesn't change the parsing (only when combined with specifyTypes) -- we simply store it, to read it later while
	// copying
	bind_data->xlsx_sheet->mHeaders = use_header;

	// If number threads > 1, we set parallel true
	if (bind_data->number_threads > 1) {
		bind_data->xlsx_file.mParallelStrings = true;
	} else {
		bind_data->xlsx_file.mParallelStrings = false;
	}

	auto start = std::chrono::system_clock::now();

	bind_data->xlsx_file.parseSharedStrings();

	auto &sheet = bind_data->xlsx_sheet;

	bool success = sheet->interleaved(bind_data->skip_rows, 0, bind_data->number_threads);
	if (!success) {
		throw BinderException("Failed to read sheet");
	}

	bind_data->xlsx_file.finalize();

	auto end = std::chrono::system_clock::now();
	std::chrono::duration<double> elapsed_seconds = end - start;
	if (bind_data->flag == 1) {
		std::cout << "Parsing time time: " << elapsed_seconds.count() << "s" << std::endl;
	}

	auto number_columns = sheet->mDimension.first;
	auto number_rows = sheet->mDimension.second;

	if (number_columns == 0 || number_rows == 0) {
		throw BinderException("Sheet appears to be empty");
	}

	vector<CellType> colTypesByIndex_first_row;
	vector<CellType> colTypesByIndex_second_row;
	// Might be used if header is present
	vector<XlsxCell> cells_first_row;

	auto first_buffer = &sheet->mCells[0].front();

	// Probing the first two rows to get the types
	if (first_buffer->size() < number_columns * 2) {
		throw BinderException(
		    "Internal SheetReader extension error: Need minimum of two rows in first buffer to determine column types");
	}

	for (idx_t i = 0; i < number_columns; i++) {
		colTypesByIndex_first_row.push_back(sheet->mCells[0].front()[i].type);
		cells_first_row.push_back(sheet->mCells[0].front()[i]);
	}

	for (idx_t i = number_columns; i < number_columns * 2; i++) {
		colTypesByIndex_second_row.push_back(sheet->mCells[0].front()[i].type);
	}

	// Convert CellType to LogicalType
	vector<LogicalType> column_types_first_row;
	vector<string> column_names_first_row;
	idx_t column_index = 0;

	bool first_row_all_string =
	    ConvertCellTypes(column_types_first_row, column_names_first_row, colTypesByIndex_first_row);

	if (use_header && !first_row_all_string) {
		throw BinderException("First row must contain only strings when has_header is set to true");
	}

	vector<LogicalType> column_types_second_row;
	vector<string> column_names_second_row;
	bool has_header = false;

	if (number_rows > 1) {
		bool second_row_all_string =
		    ConvertCellTypes(column_types_second_row, column_names_second_row, colTypesByIndex_second_row);

		if (use_header || (first_row_all_string && !second_row_all_string)) {
			has_header = true;

			return_types = column_types_second_row;
			bind_data->types = column_types_second_row;
			bind_data->SR_types = colTypesByIndex_second_row;

			vector<string> header_names;

			// Get header names from cell values of first row
			for (idx_t j = 0; j < cells_first_row.size(); j++) {
				switch (cells_first_row[j].type) {
				case CellType::T_STRING_REF: {
					auto value = bind_data->xlsx_file.getString(cells_first_row[j].data.integer);
					header_names.push_back(value);
					break;
				}
				case CellType::T_STRING:
				case CellType::T_STRING_INLINE: {
					// TODO
					throw BinderException("Inline & dynamic String types not supported yet");
					break;
				}
				default:
					throw BinderException("Header row contains non-string values");
				}
			}
			names = header_names;
			bind_data->names = header_names;
		} else {
			return_types = column_types_first_row;
			bind_data->types = column_types_first_row;
			bind_data->SR_types = colTypesByIndex_first_row;

			names = column_names_first_row;
			bind_data->names = column_names_first_row;
		}
	}

	if (has_header) {
		bind_data->skip_rows++;
		bind_data->xlsx_sheet->mSkipRows++;
	}

	// First row is discarded (is only needed for versions that use nextRow())
	for (idx_t i = 0; i < bind_data->skip_rows; i++) {
		sheet->nextRow();
	}

	return std::move(bind_data);
}

static void LoadInternal(DatabaseInstance &instance) {
	// Register a table function
	TableFunction sheetreader_table_function("sheetreader", {LogicalType::VARCHAR}, SheetreaderTableFun,
	                                         SheetreaderBindFun, SRGlobalTableFunctionState::Init,
	                                         SRLocalTableFunctionState::Init);

	sheetreader_table_function.named_parameters["sheet_name"] = LogicalType::VARCHAR;
	sheetreader_table_function.named_parameters["sheet_index"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["version"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["threads"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["flag"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["skip_rows"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["has_header"] = LogicalType::BOOLEAN;

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
