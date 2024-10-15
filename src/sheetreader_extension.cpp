#include "duckdb.h"
#include "duckdb/common/assert.hpp"
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

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#define DUCKDB_EXTENSION_MAIN

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include "sheetreader_extension.hpp"

#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

//! Determine default number of threads
inline idx_t DefaultThreads() {
	// Returns 0 if not able to detect
	idx_t sys_number_threads = std::thread::hardware_concurrency();

	// Don't be to greedy
	idx_t appropriate_number_threads = sys_number_threads / 2;

	if (appropriate_number_threads <= 0) {
		appropriate_number_threads = 1;
	}

	return appropriate_number_threads;
}

// =====================================
// Following are a bunch of constructors for classes that hold the state of the sheetreader extension
// Find the definitions & documentation of these classes in the sheetreader_extension.hpp file
// =====================================

SRBindData::SRBindData(string file_name) : SRBindData(file_name, 1) {
}

SRBindData::SRBindData(string file_name, string sheet_name)
    : xlsx_file(file_name), xlsx_sheet(make_uniq<XlsxSheet>(xlsx_file.getSheet(sheet_name))),
      number_threads(DefaultThreads()) {
}

SRBindData::SRBindData(string file_name, int sheet_index)
    : xlsx_file(file_name), xlsx_sheet(make_uniq<XlsxSheet>(xlsx_file.getSheet(sheet_index))),
      number_threads(DefaultThreads()) {
}

SRGlobalState::SRGlobalState(ClientContext &context, const SRBindData &bind_data)
    : bind_data(bind_data), chunk_count(0) {
}

SRLocalState::SRLocalState(ClientContext &context, SRGlobalState &gstate) : bind_data(gstate.bind_data) {
}

SRGlobalTableFunctionState::SRGlobalTableFunctionState(ClientContext &context, TableFunctionInitInput &input)
    : state(context, input.bind_data->Cast<SRBindData>()) {
}

unique_ptr<GlobalTableFunctionState> SRGlobalTableFunctionState::Init(ClientContext &context,
                                                                      TableFunctionInitInput &input) {

	auto result = make_uniq<SRGlobalTableFunctionState>(context, input);

	return std::move(result);
}

SRLocalTableFunctionState::SRLocalTableFunctionState(ClientContext &context, SRGlobalState &gstate)
    : state(context, gstate) {
}

unique_ptr<LocalTableFunctionState> SRLocalTableFunctionState::Init(ExecutionContext &context,
                                                                    TableFunctionInitInput &input,
                                                                    GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<SRGlobalTableFunctionState>();
	auto result = make_uniq<SRLocalTableFunctionState>(context.client, gstate.state);

	return std::move(result);
}

// =====================================
// Following are definitions that are used to copy data from the sheetreader-core to the DuckDB data chunk
// =====================================

//! DataPtr is a union that holds a pointer to the different data that are stored in the vectors of the data chunk
union DataPtr {
	string_t *string_data;
	double *double_data;
	bool *bool_data;
	date_t *date_data;
};

//! Set cell to NULL
inline void SetNull(const SRBindData &bind_data, DataChunk &output, vector<DataPtr> &flat_vectors, const XlsxCell &cell,
                    idx_t row_id, idx_t column_id) {
	LogicalType expected_type = bind_data.types[column_id];

	// Value constructor with LogicalType sets the value to NULL
	output.data[column_id].SetValue(row_id, Value(expected_type));
}

//! Set all values in the data chunk to NULL
inline void SetAllInvalid(DataChunk &output, idx_t cardinality) {
	// Iterate over all columns
	for (idx_t col = 0; col < output.ColumnCount(); col++) {
		Vector &vec = output.data[col];
		// Validity mask saves the information about NULL values
		auto &validity = FlatVector::Validity(vec);
		validity.SetAllInvalid(cardinality);
	}
}

//! Set cell to the value of XlsxCell
//! Expects XlsxCell to have the same type as the column
inline void SetCell(const SRBindData &bind_data, DataChunk &output, vector<DataPtr> &flat_vectors, const XlsxCell &cell,
                    idx_t row_id, idx_t column_id) {

	auto &xlsx_file = bind_data.xlsx_file;

	// Get validity mask of the column and set it to valid (i.e. not NULL)
	Vector &vec = output.data[column_id];
	auto &validity = FlatVector::Validity(vec);
	validity.SetValid(row_id);

	// Set the value of the cell to cell in the data chunk
	// Note: bind_data.types[column_id] is the expected type of the column,
	// so the type XlsxCell should be checked before calling this function
	switch (bind_data.types[column_id].id()) {
	case LogicalTypeId::VARCHAR: {
		auto value = xlsx_file.getString(cell.data.integer);
		// string_t creates values that fail the UTF-8 check, so we use the slow technique
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
		// Convert seconds to days
		date_t value = date_t((int)(cell.data.real / 86400.0));
		flat_vectors[column_id].date_data[row_id] = value;
		break;
	}
	default:
		throw InternalException("This shouldn't happen. Unsupported Logical type");
	}
}

//! Coerce cell to string and save it in the data chunk
inline void SetCellString(const SRBindData &bind_data, DataChunk &output, vector<DataPtr> &flat_vectors,
                          const XlsxCell &cell, idx_t row_id, idx_t column_id) {

	auto &xlsx_file = bind_data.xlsx_file;

	// Get validity mask of the column and set it to valid (i.e. not NULL)
	Vector &vec = output.data[column_id];
	auto &validity = FlatVector::Validity(vec);
	validity.SetValid(row_id);

	// Similar to SetCell() only difference:
	// Use coercion method depending on the type of the XlsxCell
	switch (cell.type) {
	case CellType::T_STRING_REF: {
		auto value = xlsx_file.getString(cell.data.integer);
		output.data[column_id].SetValue(row_id, Value(value));
		break;
	}
	case CellType::T_NUMERIC: {
		auto value = cell.data.real;
		string str = std::to_string(value);
		output.data[column_id].SetValue(row_id, Value(str));
		break;
	}
	case CellType::T_BOOLEAN: {
		auto value = cell.data.boolean;
		string str = value ? "TRUE" : "FALSE";
		output.data[column_id].SetValue(row_id, Value(str));
		break;
	}
	case CellType::T_DATE: {
		date_t value = date_t((int)(cell.data.real / 86400.0));
		string str = Date::ToString(value);
		output.data[column_id].SetValue(row_id, Value(str));
		break;
	}
	default:
		throw InternalException("This shouldn't happen. Unsupported Cell type");
	}
}

//! Check if the types of the XlsxCell and the column are compatible
//! Types are compatible with VARCHAR if coercing to string is enabled
bool TypesCompatible(const LogicalType &expected_type, const CellType &cell_type, bool coerce_to_string) {
	switch (expected_type.id()) {
	case LogicalTypeId::VARCHAR:
		if (coerce_to_string) {
			switch (cell_type) {
			case CellType::T_STRING_REF:
			case CellType::T_NUMERIC:
			case CellType::T_BOOLEAN:
			case CellType::T_DATE:
				return true;
			default:
				return false;
			}
		}
		return cell_type == CellType::T_STRING_REF;
	case LogicalTypeId::DOUBLE:
		return cell_type == CellType::T_NUMERIC;
	case LogicalTypeId::BOOLEAN:
		return cell_type == CellType::T_BOOLEAN;
	case LogicalTypeId::DATE:
		return cell_type == CellType::T_DATE;
	default:
		// TODO: Add support for T_STRING and T_STRING_INLINE
		throw InternalException("This shouldn't happen. Unsupported Logical type");
	}
}

//! Check if current_row is within the limit of the current chunk
bool CheckRowLimitReached(SRGlobalState &gstate) {
	// Need offset, since current_row is index of the current row in whole table.
	// So we subtract the number of rows (determined by chunk) already copied
	long long row_offset = gstate.chunk_count * STANDARD_VECTOR_SIZE;
	// Limit is the last row of the current chunk (should be 2048 == STANDARD_VECTOR_SIZE)
	long long limit = row_offset + STANDARD_VECTOR_SIZE;
	long long skip_rows = gstate.bind_data.xlsx_sheet->mSkipRows;
	bool limit_reached = gstate.current_row - skip_rows >= limit;
	return limit_reached;
}

//! Get the number of rows copied so far
idx_t GetCardinality(SRGlobalState &gstate) {
	// Same reason as in CheckRowLimitReached
	long long row_offset = gstate.chunk_count * STANDARD_VECTOR_SIZE;
	long long skip_rows = gstate.bind_data.xlsx_sheet->mSkipRows;
	// This is the case when no new rows are copied and last chunk was not full (last iteration)
	if (gstate.current_row + 1 < skip_rows + row_offset) {
		return 0;
	}
	return gstate.current_row - skip_rows - row_offset + 1;
}

/*!
  Summary of data is stored in mCells:
    ====================================

    General layout:
    ---------------

  mCells = [thread[0], thread[1], thread[2], ...]
  thread[current_thread] = [buffer[0], buffer[1], buffer[2], ...]
  buffer[current_buffer] = [cell[0], cell[1], cell[2], ...]
*/
/*!
   - Copy data from sheetreader-core's mCells to DuckDB data chunk
   - Copies STANDARD_VECTOR_SIZE rows at a time
   - This function is stateful, so it keeps track of the current location in the mCells
     to maintain state between calls
   Returns Cardinality (number of rows copied)
*/
size_t StatefulCopy(SRGlobalState &gstate, const SRBindData &bind_data, DataChunk &output,
                    vector<DataPtr> &flat_vectors) {

	auto &sheet = bind_data.xlsx_sheet;

	// Every thread has a list of buffers
	D_ASSERT(bind_data.number_threads == sheet->mCells.size());

	idx_t number_threads = bind_data.number_threads;

	if (number_threads == 0) {
		return 0;
	}

	size_t row_offset = gstate.chunk_count * STANDARD_VECTOR_SIZE;

	// Helper function to calculate the adjusted row
	auto calc_adjusted_row = [row_offset](long long current_row, unsigned long skip_rows) {
		return current_row - skip_rows - row_offset;
	};

	// Initialize state for first call
	if (gstate.current_locs.empty()) {
		// Get number of buffers from first thread (is always the maximum)
		gstate.max_buffers = sheet->mCells[0].size();
		gstate.current_thread = 0;
		gstate.current_buffer = 0;
		gstate.current_cell = 0;
		gstate.current_column = 0;
		gstate.current_row = -1;
		// Initialize current_locs for all threads
		gstate.current_locs = std::vector<size_t>(number_threads, 0);
	}

	// Set all values to NULL per default, since sheetreader-core stores information about empty cells only by skipping
	// them in mCells. Since we iterate over mCells, empty cells are implicitly skipped. So we wouldn't know if a cell
	// in the chunk is empty if we don't set it to NULL here and set it to valid when we find it in mCells (see
	// SetValue)
	SetAllInvalid(output, STANDARD_VECTOR_SIZE);

	//! To get the correct order of rows we iterate for(buffer_index) { for(thread_index) { for(cell_index) } }
	//! This is due to how sheetreader-core writes the data to the buffers (stored in mCells)
	for (; gstate.current_buffer < gstate.max_buffers; ++gstate.current_buffer) {
		for (; gstate.current_thread < sheet->mCells.size(); ++gstate.current_thread) {

			// If there are no more buffers to read, prepare for finishing copying
			if (sheet->mCells[gstate.current_thread].empty()) {
				// Set to maxBuffers, so this is the last iteration
				gstate.current_buffer = gstate.max_buffers;

				// Return number of copied rows in this chunk
				return GetCardinality(gstate);
			}

			//! Current cell buffer
			const std::vector<XlsxCell> cells = sheet->mCells[gstate.current_thread].front();
			//! Location info for current thread
			const std::vector<LocationInfo> &locs_infos = sheet->mLocationInfos[gstate.current_thread];
			//! Current location index in current thread
			size_t &current_loc = gstate.current_locs[gstate.current_thread];

			// This is a weird implementation detail of sheetreader-core:
			// currentCell <= cells.size() because there might be location info after last cell
			for (; gstate.current_cell <= cells.size(); ++gstate.current_cell) {

				// Description of the following loop:
				// Update currentRow & currentColumn when location info is available for current cell at currentLoc.
				// After setting those values: Advance to next location info.
				//
				// This means that the values won't be updated if there is no location info for the current cell
				// (e.g. not first cell in row)
				//
				// Edge case 0:
				// Loop is executed n+1 times for first location info, where n is the number of skip_rows (specified as
				// parameter for interleaved) This is because, SheetReader creates location infos for the skipped lines
				// with cell == column == buffer == 0 sames as for the first "real" row
				//
				// Edge case 1:
				// For empty cells, sheetreader-core also generates a location info that points to the same cell as the
				// next location info. By using the condition for the while loop, we skip these empty cells
				while (current_loc < locs_infos.size() && locs_infos[current_loc].buffer == gstate.current_buffer &&
				       locs_infos[current_loc].cell == gstate.current_cell) {

					gstate.current_column = locs_infos[current_loc].column;
					// Not sure whether row is ever -1ul, but this is how it's handled in sheetreader-core's nextRow()
					if (locs_infos[current_loc].row == -1ul) {
						++gstate.current_row;
					} else {
						gstate.current_row = locs_infos[current_loc].row;
					}

					long long adjusted_row = calc_adjusted_row(gstate.current_row, sheet->mSkipRows);

					// This only happens for header rows -- we want to skip them
					if (adjusted_row < 0) {
						++current_loc;
						// Skip to next row
						if (current_loc < locs_infos.size()) {
							gstate.current_cell = locs_infos[current_loc].cell;
						} else {
							throw InternalException("Skipped more rows than available in first buffer -- consider "
							                        "decreasing number of threads");
						}
						continue;
					}

					// Increment index to location info for next iteration
					++current_loc;

					// If we reached the row limit of the current chunk, we return the number of copied rows
					if (CheckRowLimitReached(gstate)) {
						// Subtract 1, because we increment current_row before checking the limit
						return (GetCardinality(gstate) - 1);
					}
				}
				// We need to check this here, because we iterate up to cells.size() to get the last location info
				if (gstate.current_cell >= cells.size()) {
					break;
				}

				// Use short variable name for better readability
				const auto current_column = gstate.current_column;

				// If this cell is in a column that was not present in the first row, we throw an error
				if (current_column >= bind_data.types.size()) {
					throw InvalidInputException(
					    "Row " + std::to_string(gstate.current_row) + "has more columns than the first row. Has: " +
					    std::to_string(current_column + 1) + " Expected: " + std::to_string(bind_data.types.size()));
				}

				//! Content of current cell
				const XlsxCell &cell = cells[gstate.current_cell];
				//! Number of rows we skipped while parsing
				long long mSkipRows = sheet->mSkipRows;
				long long adjusted_row = calc_adjusted_row(gstate.current_row, mSkipRows);

				bool types_compatible =
				    TypesCompatible(bind_data.types[current_column], cell.type, bind_data.coerce_to_string);

				// sheetreader-core doesn't determine empty cells to be T_NONE, instead it skips the cell,
				// so it's not stored in mCells. We handle this by setting all cells as Invalid (aka null)
				// and set them valid when they appear in mCells
				if (cell.type == CellType::T_NONE || cell.type == CellType::T_ERROR || !types_compatible) {
					SetNull(bind_data, output, flat_vectors, cell, adjusted_row, current_column);
				} else if (bind_data.types[current_column] == LogicalType::VARCHAR && bind_data.coerce_to_string) {
					SetCellString(bind_data, output, flat_vectors, cell, adjusted_row, current_column);
				} else {
					SetCell(bind_data, output, flat_vectors, cell, adjusted_row, current_column);
				}

				// Advance to next column
				++gstate.current_column;
			}

			// If we reached the last cell in the current buffer, we remove it from the thread
			sheet->mCells[gstate.current_thread].pop_front();
			// Reset for next buffer
			gstate.current_cell = 0;
		}
		// Reset thread index for next buffer index
		gstate.current_thread = 0;
	}
	// Return number of copied rows in this chunk when all buffers are read (i.e. curren_buffer == max_buffers)
	return GetCardinality(gstate);
}

//! Finish the current chunk
//! - Set the cardinality of the chunk
//! - Increment the chunk count
inline void FinishChunk(DataChunk &output, idx_t cardinality, SRGlobalState &gstate) {

	// Indicate how many rows are in the chunk
	// If cardinality is 0, it means that the chunk is empty and no more rows are to be expected
	output.SetCardinality(cardinality);

	// Increment number of chunks read so far
	gstate.chunk_count++;

	return;
}

//! Copy data from sheetreader-core to DuckDB data chunk
//! - Is called after bind function
//! - Is called multiple times until all data is copied are no more rows are needed (e.g. for LIMIT clause)
inline void SheetreaderCopyTableFun(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {

	//! Data from bind function
	const SRBindData &bind_data = data_p.bind_data->Cast<SRBindData>();
	//! State persisted in between table (copy) function calls
	SRGlobalState &gstate = data_p.global_state->Cast<SRGlobalTableFunctionState>().state;

	//! Number of columns (i.e. number of vectors in the data chunk)
	const idx_t column_count = output.ColumnCount();

	D_ASSERT(column_count == bind_data.types.size());

	// =====================================
	// Store FlatVectors for all columns (they have different data types)
	// =====================================

	//! Holds pointers to the data of the vectors in the data chunk
	vector<DataPtr> flat_vectors;

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

	// =====================================
	// Copy data from sheetreader-core's mCells to DuckDB data chunk
	// =====================================

	// This version:
	// - Uses SetValue only for VARCHAR, for other types it uses directly the flat vectors
	// - Doesn't use nextRow() but directly iterates over the buffers
	// - Has more features (coercion to string, handling empty cells, etc.)

	//! Number of rows copied in this iteration
	auto cardinality = StatefulCopy(gstate, bind_data, output, flat_vectors);

	FinishChunk(output, cardinality, gstate);

	return;
}

// =====================================
// Following are definitions for the bind function
// =====================================

//! Converts the cell types from sheetreader-core to DuckDB types (column_types)
//! and it also sets the column names (uses generic names)
inline bool ConvertCellTypes(vector<LogicalType> &column_types, vector<string> &column_names,
                             vector<CellType> &cell_types) {
	idx_t current_column_index = 0;
	//! Indicates if the first row contains only string values
	bool first_row_all_string = true;

	for (auto &col_type : cell_types) {
		switch (col_type) {
		case CellType::T_STRING_REF:
			column_types.push_back(LogicalType::VARCHAR);
			column_names.push_back("String" + std::to_string(current_column_index));
			break;
		case CellType::T_STRING:
		case CellType::T_STRING_INLINE:
			// TODO
			throw BinderException("Inline & dynamic String types not supported yet");
			break;
		case CellType::T_NUMERIC:
			column_types.push_back(LogicalType::DOUBLE);
			column_names.push_back("Numeric" + std::to_string(current_column_index));
			first_row_all_string = false;
			break;
		case CellType::T_BOOLEAN:
			column_types.push_back(LogicalType::BOOLEAN);
			column_names.push_back("Boolean" + std::to_string(current_column_index));
			first_row_all_string = false;
			break;
		case CellType::T_DATE:
			column_types.push_back(LogicalType::DATE);
			column_names.push_back("Date" + std::to_string(current_column_index));
			first_row_all_string = false;
			break;
		default:
			throw BinderException("Unknown cell type in column in column " + std::to_string(current_column_index));
		}
		current_column_index++;
	}

	return first_row_all_string;
}

//! Get the names of the columns from the first row
//! Assumes that the first row contains only string values
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

//! Bind function for the sheetreader extension
//! - Gets (named) parameters (filename etc.) of table function and stores them
//! - Parses the .Xlsx-file
//! - Reads the first & second row to determine the types of the columns
//! - Reads the first & second row to determine the names of the columns (auto detects if the first row is a header)
//! - Writes the determined types in `return_types` and the names in `names`
//! - Creates the bind data object (is subtype of FunctionData) which contains all necessary information for the copy
//!   and most importantly stores the XlsxFile & XlsxSheet objects
inline unique_ptr<FunctionData> SheetreaderBindFun(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {

	// =====================================
	// Get input parameters & prepare for parsing
	// =====================================

	// Get the file name from the input parameters & verify it exists
	auto file_reader = MultiFileReader::Create(input.table_function);
	auto file_list = file_reader->CreateFileList(context, input.inputs[0]);
	auto file_names = file_list->GetAllFiles();

	if (file_names.empty()) {
		throw BinderException("No files found in path");
	} else if (file_names.size() > 1) {
		throw BinderException("Only one file can be read at a time");
	}

	//! User specified sheet name
	string sheet_name;
	//! User specified sheet index -- starts with 1
	int sheet_index;
	//! Is set when the user specifies the sheet index with e.g. `sheet_index=2`
	bool sheet_index_set = false;

	//! User specified option to use header
	bool use_header = false;

	// Get named parameters that are needed for creating XlsxFile & XlsxSheet objects and therefore for creating
	// bind_data
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

	//! Contains all important data collected in this bind function & is returned to be used by table (copy) function
	unique_ptr<SRBindData> bind_data;

	try {
		if (!sheet_name.empty()) {
			bind_data = make_uniq<SRBindData>(file_names[0], sheet_name);
		} else if (sheet_index_set) {
			bind_data = make_uniq<SRBindData>(file_names[0], sheet_index);
		} else {
			// Default: sheet_index=1
			bind_data = make_uniq<SRBindData>(file_names[0]);
		}
	} catch (std::exception &e) {
		throw BinderException(e.what());
	}

	//! Is set when the user specifies the types of the columns with e.g. `types=[VARCHAR,DOUBLE]`
	bool has_user_types = false;

	// Get all left named parameters
	// You can find the documentation of the named parameters in the README.md and header file
	for (auto &kv : input.named_parameters) {
		auto loption = StringUtil::Lower(kv.first);
		if (loption == "threads") {
			bind_data->number_threads = IntegerValue::Get(kv.second);
			if (bind_data->number_threads <= 0) {
				throw BinderException("Number of threads must be greater than 0");
			}
		} else if (loption == "skip_rows") {
			// Default: 0
			bind_data->skip_rows = IntegerValue::Get(kv.second);
		} else if (loption == "coerce_to_string") {
			bind_data->coerce_to_string = BooleanValue::Get(kv.second);
		} else if (loption == "force_types") {
			bind_data->force_types = BooleanValue::Get(kv.second);
		} else if (loption == "types") {
			// Get all types as strings defined in list/array
			auto &children = ListValue::GetChildren(kv.second);
			// Convert strings to LogicalTypes & check if they are supported
			for (auto &child : children) {
				string raw_type = StringValue::Get(child);
				LogicalType logical_type = TransformStringToLogicalType(raw_type);
				if (logical_type.id() == LogicalTypeId::USER) {
					throw BinderException("Unrecognized type \"%s\" for %s definition", raw_type, kv.first);
				}
				switch (logical_type.id()) {
				case LogicalTypeId::VARCHAR:
				case LogicalTypeId::DOUBLE:
				case LogicalTypeId::BOOLEAN:
				case LogicalTypeId::DATE: {
					break;
				}
				default: {
					throw BinderException("Unsupported type \"%s\" for %s definition", raw_type, kv.first);
				}
				}
				bind_data->user_types.push_back(logical_type);
			}
			// Indicate that user provided types
			has_user_types = true;

			// We already handled them before
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

	// =====================================
	// Parsing & check parsing result
	// =====================================

	// Parse the shared strings file
	bind_data->xlsx_file.parseSharedStrings();

	//! Used for better readability
	auto &sheet = bind_data->xlsx_sheet;

	// Parse the sheet
	bool success = sheet->interleaved(bind_data->skip_rows, 0, bind_data->number_threads);

	if (!success) {
		throw BinderException("Failed to read sheet");
	}

	bind_data->xlsx_file.finalize();

	//! Number of columns in the sheet
	auto number_columns = sheet->mDimension.first;
	//! Number of rows in the sheet
	auto number_rows = sheet->mDimension.second;

	if (number_columns == 0 || number_rows == 0) {
		throw BinderException("Sheet appears to be empty");
	}

	// =====================================
	// Determine column types & names
	// =====================================

	// First buffer of first thread
	auto first_buffer = &sheet->mCells[0].front();

	// Check preconditions for determining column types:
	//
	// 0. Need minimum of two rows to determine column types
	if (number_rows < 2) {
		throw BinderException("Need minimum of two rows to determine column types");
	}
	//
	// 1. Check that nothing weird happened due to interleaved reading
	if (first_buffer->size() < number_columns * 2) {
		throw BinderException("Internal SheetReader extension error: Need minimum of two rows in first buffer to "
		                      "determine column types and auto detect header row");
	}

	//! Cell types in the first row after skipped rows
	vector<CellType> cell_types_first_row;
	//! Cell types in the second row after skipped rows
	vector<CellType> cell_types_second_row;
	//! Cell values in the first row after skipped rows
	vector<XlsxCell> cells_first_row;

	// Probing the first two rows to get the types
	for (idx_t i = 0; i < number_columns; i++) {
		cell_types_first_row.push_back(sheet->mCells[0].front()[i].type);
		cells_first_row.push_back(sheet->mCells[0].front()[i]);
	}

	for (idx_t i = number_columns; i < number_columns * 2; i++) {
		cell_types_second_row.push_back(sheet->mCells[0].front()[i].type);
	}

	// Convert CellType to LogicalType

	//! DuckDB types of the cells in the first row
	vector<LogicalType> column_types_first_row;
	//! Column names of the cells in the first row
	vector<string> column_names_first_row;

	// Check if first row contains only string values, get DuckDB types & generic column names
	bool first_row_all_string = ConvertCellTypes(column_types_first_row, column_names_first_row, cell_types_first_row);

	if (use_header && !first_row_all_string) {
		throw BinderException("First row must contain only strings when has_header is set to true");
	}

	vector<LogicalType> column_types_second_row;
	vector<string> column_names_second_row;

	//! Indicates whether a header row was detected
	bool header_detected = false;

	// Check if second row contains only string values, get DuckDB types & generic column names
	bool second_row_all_string =
	    ConvertCellTypes(column_types_second_row, column_names_second_row, cell_types_second_row);

	// If the first row contains only string values, but the second row doesn't, we assume that the first row is a
	// header row
	if (use_header || (first_row_all_string && !second_row_all_string)) {
		header_detected = true;

		// Since the first row is a header row, we use the cell types of the second row
		return_types = column_types_second_row;
		bind_data->types = column_types_second_row;

		//! Column names determined from the first row
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

		// Set column names to header names
		names = header_names;
		bind_data->names = header_names;
	} else {
		// If first row is not a header row, we use the cell types of the first row for the column types
		return_types = column_types_first_row;
		bind_data->types = column_types_first_row;

		// Use generic column names
		names = column_names_first_row;
		bind_data->names = column_names_first_row;
	}

	// Since header is only used for determining column names, we skip it
	if (header_detected) {
		bind_data->skip_rows++;
		bind_data->xlsx_sheet->mSkipRows++;
	}

	// If user has specified types, we try to use them
	if (has_user_types) {
		if (bind_data->user_types.size() < number_columns) {
			throw BinderException("Number of user defined types is less than number of columns in sheet");
		}

		idx_t column_index = 0;
		for (auto &column_type : return_types) {

			LogicalType user_type = bind_data->user_types[column_index];

			// Check if user defined type is same as previously determined column type or can be coerced to string
			// If forced_types == true, the compatibility check is skipped
			if (!bind_data->force_types && user_type.id() != column_type.id() &&
			    !(user_type == LogicalTypeId::VARCHAR && bind_data->coerce_to_string)) {
				// TODO: EnumUtil does not work -- find appropriate replacement
				// throw BinderException("User defined type %s for column with index %d is not compatible with actual
				// type %s",
				//                       EnumUtil::ToString<LogicalType>(user_type), column_index,
				//                       EnumUtil::ToString<LogicalType>(column_type));
				throw BinderException("User defined type for column with index %d is not compatible with actual type",
				                      column_index);
			}
			column_index++;
		}

		// Add column names, if they are new user defined columns
		vector<string> additional_column_names;

		while (column_index < bind_data->user_types.size()) {
			additional_column_names.push_back("Column " + std::to_string(column_index));
			column_index++;
		}

		return_types = bind_data->user_types;
		bind_data->types = bind_data->user_types;

		// Concat additional column names
		bind_data->names.insert(bind_data->names.end(), additional_column_names.begin(), additional_column_names.end());
		names = bind_data->names;

		D_ASSERT(return_types.size() == names.size());
	}

	// First row is discarded (is only needed for versions that use nextRow())
	for (idx_t i = 0; i < bind_data->skip_rows; i++) {
		sheet->nextRow();
	}

	return std::move(bind_data);
}

static void LoadInternal(DatabaseInstance &instance) {
	// Register a table function
	TableFunction sheetreader_table_function("sheetreader", {LogicalType::VARCHAR}, SheetreaderCopyTableFun,
	                                         SheetreaderBindFun, SRGlobalTableFunctionState::Init,
	                                         SRLocalTableFunctionState::Init);

	// Define all named parameters
	sheetreader_table_function.named_parameters["sheet_name"] = LogicalType::VARCHAR;
	sheetreader_table_function.named_parameters["sheet_index"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["threads"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["skip_rows"] = LogicalType::INTEGER;
	sheetreader_table_function.named_parameters["has_header"] = LogicalType::BOOLEAN;
	// TODO: Support STRUCT, i.e. { 'column_name': 'type', ... }
	// We would use ANY here, similar to read_csv.cpp, but we expect a STRUCT or LIST
	// sheetreader_table_function.named_parameters["types"] = LogicalType::ANY;
	sheetreader_table_function.named_parameters["types"] = LogicalType::LIST(LogicalType::VARCHAR);
	sheetreader_table_function.named_parameters["force_types"] = LogicalType::BOOLEAN;
	sheetreader_table_function.named_parameters["coerce_to_string"] = LogicalType::BOOLEAN;

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
