#pragma once

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
	vector<string> files;
	
	// Filereader

	//! Whether or not we should ignore malformed JSON (default to NULL)
	bool ignore_errors = false;
	//! Maximum JSON object size (defaults to 16MB minimum)
	idx_t maximum_object_size = 16777216;
	//! Whether we auto-detect a schema
	bool auto_detect = false;
	//! Sample size for detecting schema
	idx_t sample_size = idx_t(STANDARD_VECTOR_SIZE) * 10;
	//! Max depth we go to detect nested JSON schema (defaults to unlimited)
	idx_t max_depth = NumericLimits<idx_t>::Maximum();
	//! We divide the number of appearances of each JSON field by the auto-detection sample size
	//! If the average over the fields of an object is less than this threshold,
	//! we default to the JSON type for this object rather than the shredded type
	double field_appearance_threshold = 0.1;
	//! The maximum number of files we sample to sample sample_size rows
	idx_t maximum_sample_files = 32;
	//! Whether we auto-detect and convert JSON strings to integers
	bool convert_strings_to_integers = false;

	//! All column names (in order)
	vector<string> names;

	//! The inferred avg tuple size
	idx_t avg_tuple_size = 420;
private:
	SheetreaderScanData(ClientContext &context, vector<string> files);

};
} // namespace duckdb
