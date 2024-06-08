#include "XlsxFile.h"

#include <stdexcept>
#include <map>
#include <set>

#include "XlsxSheet.h"
#include "parsing.h"

XlsxFile::XlsxFile(const std::string archivePath)
    : mArchivePath(archivePath)
    , mFile(nullptr)
    , mFileSharedStrings(nullptr)
    , mPathSharedStrings("")
    , mPathStyles("")
    , mDate1904(false)
    , mParallelStrings(false)
{
    mFile = new mz_zip_archive;
    memset(mFile, 0, sizeof(*mFile));

    mz_bool status = mz_zip_reader_init_file(mFile, mArchivePath.c_str(), 0);
    if (!status) {
        mz_zip_error err = mFile->m_last_error;
        delete mFile;
        mFile = nullptr;
        if (err == MZ_ZIP_FILE_OPEN_FAILED) {
            throw std::invalid_argument("Unable to open file '" + mArchivePath + "'");
        }
        throw std::invalid_argument("Failed to initalize file " + std::to_string(err));
    }

    parseRootRelationships();
    parseWorkbook();
    parseWorkbookRelationships();
    if (mPathStyles != "") parseStyles();
}

XlsxFile::~XlsxFile() {
    if (mFile != nullptr) {
        mz_zip_reader_end(mFile);
        delete mFile;
    }
    if (mFileSharedStrings != nullptr) {
        mz_zip_reader_end(mFileSharedStrings);
        delete mFileSharedStrings;
    }
}

int fileIndex(mz_zip_archive* archive, const char* fileName) {
    // tries to retrieve the file index
    // matches even if preceding '/' does not
    mz_zip_archive_file_stat fileStat;
    const int offset = fileName[0] == '/' ? 1 : 0;
    for (int i = 0; i < (int)mz_zip_reader_get_num_files(archive); ++i) {
        if (mz_zip_reader_file_stat(archive, i, &fileStat)) {
            if (strcmp(fileStat.m_filename + (fileStat.m_filename[0] == '/' ? 1 : 0), fileName + offset) == 0) {
                return i;
            }
        }
    }
    return -1;
}

void XlsxFile::parseRootRelationships() {
    const int relIndex = fileIndex(mFile, "_rels/.rels");
    if (relIndex < 0) {
        throw std::runtime_error("Failed to find root rel file");
    }
    mz_zip_archive_file_stat fileStat;
    mz_zip_reader_file_stat(mFile, relIndex, &fileStat);

    size_t uncompSize;
    void* p = mz_zip_reader_extract_to_heap(mFile, relIndex, &uncompSize, 0);
    if (!p) {
        throw std::runtime_error("Failed to extract root rel file");
    }

    ElementParser<2> relationship("Relationship", {"Target", "Type"}, {AttributeType::STRING, AttributeType::STRING});

    int errors = 0;
    for (size_t i = 0; i < uncompSize; ++i) {
        const unsigned char current = static_cast<const unsigned char*>(p)[i];

        relationship.process(current);
        if (relationship.completed()) {
            if (!relationship.hasValue(0) || !relationship.hasValue(1)) {
                ++errors;
                continue;
            }
            const std::string type = static_cast<const StringParser&>(relationship.getAttribute(1)).getValue();
            if (type.length() > 14 && type.substr(type.length() - 14) == "officeDocument") {
                const std::string target = static_cast<const StringParser&>(relationship.getAttribute(0)).getValue();
                mPathWorkbook = target.at(0) == '/' ? target.substr(1) : target;
            }
        }
    }
    mz_free(p);
    if (errors > 0 && mPathWorkbook == "") {
        throw std::runtime_error("Failed to retrieve path for workbook");
    }
}

void XlsxFile::parseWorkbook() {
    if (mPathWorkbook == "") {
        throw std::runtime_error("Invalid workbook path");
    }
    const int wbIndex = fileIndex(mFile, mPathWorkbook.c_str());
    if (wbIndex < 0) {
        throw std::runtime_error("Failed to find workbook file");
    }
    mz_zip_archive_file_stat fileStat;
    mz_zip_reader_file_stat(mFile, wbIndex, &fileStat);

    size_t uncompSize;
    void* p = mz_zip_reader_extract_to_heap(mFile, wbIndex, &uncompSize, 0);
    if (!p) {
        throw std::runtime_error("Failed to extract workbook file");
    }

    ElementParser<0> sheets("sheets", {}, {});
    ElementParser<3> sheet("sheet", {"name", "sheetId", "id"}, {AttributeType::STRING, AttributeType::INDEX, AttributeType::STRING});
    ElementParser<1> workbookPr("workbookPr", {"date1904"}, {AttributeType::STRING});

    for (size_t i = 0; i < uncompSize; ++i) {
        const unsigned char current = static_cast<const unsigned char*>(p)[i];

        sheets.process(current);
        if (sheets.inside()) {
            sheet.process(current);
            if (sheet.completed()) {
                if (!sheet.hasValue(0) || !sheet.hasValue(1) || !sheet.hasValue(2)) {
                    // incomplete sheet index parse
                    //TODO: not sure what to do about this, maybe check if the sheet requested from user was retrieved?
                    continue;
                }
                mSheetIndex.emplace_back(
                    static_cast<const IndexParser&>(sheet.getAttribute(1)).getValue(),
                    unescape(static_cast<const StringParser&>(sheet.getAttribute(0)).getValue()),
                    static_cast<const StringParser&>(sheet.getAttribute(2)).getValue(),
                    ""
                );
            }
        }
        workbookPr.process(current);
        if (workbookPr.completed()) {
            if (workbookPr.hasValue(0)) {
                const std::string val = static_cast<const StringParser&>(workbookPr.getAttribute(0)).getValue();
                if (val != "false" && val != "0") mDate1904 = true;
            }
        }
    }

    mz_free(p);
    if (mSheetIndex.size() == 0) {
        throw std::runtime_error("Failed to retrieve sheet paths");
    }
}

void XlsxFile::parseWorkbookRelationships() {
    // based on the workbook path
    if (mPathWorkbook == "") {
        throw std::runtime_error("Invalid workbook rel path");
    }
    const size_t lastSlash = mPathWorkbook.find_last_of('/');
    // account for rare case where workbook is in top level
    const std::string localPath = mPathWorkbook.substr(0, lastSlash == std::string::npos ? 0 : lastSlash + 1);
    const std::string relPath = localPath + "_rels/workbook.xml.rels";

    const int relIndex = fileIndex(mFile, relPath.c_str());
    if (relIndex < 0) {
        throw std::runtime_error("Failed to find workbook rel file " + relPath);
    }
    mz_zip_archive_file_stat fileStat;
    mz_zip_reader_file_stat(mFile, relIndex, &fileStat);

    size_t uncompSize;
    void* p = mz_zip_reader_extract_to_heap(mFile, relIndex, &uncompSize, 0);
    if (!p) {
        throw std::runtime_error("Failed to extract workbook rel file");
    }

    ElementParser<3> relationship("Relationship", {"Target", "Type", "Id"}, {AttributeType::STRING, AttributeType::STRING, AttributeType::STRING});

    int errors = 0;
    for (size_t i = 0; i < uncompSize; ++i) {
        const unsigned char current = static_cast<const unsigned char*>(p)[i];

        relationship.process(current);
        if (relationship.completed()) {
            if (!relationship.hasValue(0) || !relationship.hasValue(1) || !relationship.hasValue(2)) {
                ++errors;
                continue;
            }
            const std::string type = static_cast<const StringParser&>(relationship.getAttribute(1)).getValue();
            if (type.length() >= 10 && type.substr(type.length() - 10) == "/worksheet") {
                // get archive locations of sheets
                const std::string id = static_cast<const StringParser&>(relationship.getAttribute(2)).getValue();

                for (size_t j = 0; j < mSheetIndex.size(); ++j) {
                    if (id == std::get<2>(mSheetIndex[j])) {
                        std::string target = static_cast<const StringParser&>(relationship.getAttribute(0)).getValue();
                        if (target.at(0) == '/') {
                            // absolute
                            target = target.substr(1);
                        } else {
                            // relative, make absolute
                            target = localPath + target;
                        }
                        std::get<3>(mSheetIndex[j]) = target;
                    }
                }
            } else if (type.length() >= 14 && type.substr(type.length() - 14) == "/sharedStrings") {
                // location for shared strings file
                std::string target = static_cast<const StringParser&>(relationship.getAttribute(0)).getValue();
                target = target.at(0) == '/' ? target = target.substr(1) : target = localPath + target;
                mPathSharedStrings = target;
            } else if (type.length() >= 7 && type.substr(type.length() - 7) == "/styles") {
                // location for the styles file
                std::string target = static_cast<const StringParser&>(relationship.getAttribute(0)).getValue();
                target = target.at(0) == '/' ? target = target.substr(1) : target = localPath + target;
                mPathStyles = target;
            }
        }
    }

    mz_free(p);

    if (errors > 0) {
        bool sheetPaths = true;
        for (size_t i = 0; i < mSheetIndex.size(); ++i) {
            if (std::get<3>(mSheetIndex[i]) == "") {
                sheetPaths = false;
                break;
            }
        }
        //TODO: maybe sheetPaths only if its the sheet requested from user?
        if (mPathSharedStrings == "" || mPathStyles == "" || sheetPaths) {
            throw std::runtime_error("Errors while reading workbook relationships");
        }
    }
}

void XlsxFile::parseStyles() {
    const int relIndex = fileIndex(mFile, mPathStyles.c_str());
    if (relIndex < 0) {
        throw std::runtime_error("Failed to find styles file");
    }

    mz_zip_reader_extract_iter_state* state = mz_zip_reader_extract_iter_new(mFile, relIndex, 0);
    if (!state) {
        throw std::runtime_error("Failed to initialize reader state for styles");
    }

    constexpr size_t bufferSize = 32768;
    char buffer[bufferSize];
    size_t offset = 0;
    size_t read = 0;

    ElementParser<0> cellXfs("cellXfs", {}, {});
    ElementParser<1> xf("xf", {"numFmtId"}, {AttributeType::INDEX});
    ElementParser<0> numFmts("numFmts", {}, {});
    ElementParser<2> numFmt("numFmt", {"numFmtId", "formatCode"}, {AttributeType::INDEX, AttributeType::STRING});

    std::map<unsigned long, unsigned long> xfMapping;
    unsigned long xfCount = 0;
    std::set<unsigned long> customDateFormats;

    while (true) {
        if (offset >= read) {
            mz_zip_error err = MZ_ZIP_NO_ERROR;
            read = mz_zip_reader_extract_iter_read(state, buffer, bufferSize, err);
            if (state->status < 0) {
                mz_zip_reader_extract_iter_free(state);
                throw std::runtime_error("Error while decompressing styles file");
            }
            if (read == 0) break;
            offset = 0;
        }
        const unsigned char current = buffer[offset];
        ++offset;

        cellXfs.process(current);
        if (cellXfs.inside()) {
            xf.process(current);
            if (xf.completed()) {
                if (!xf.hasValue(0)) {
                    //TODO: incomplete xf parse, what to do?
                    ++xfCount;
                    continue;
                }
                xfMapping.emplace(xfCount++, static_cast<const IndexParser&>(xf.getAttribute(0)).getValue());
            }
        }
        numFmts.process(current);
        if (numFmts.inside()) {
            numFmt.process(current);
            if (numFmt.completed()) {
                if (!numFmt.hasValue(0) || !numFmt.hasValue(1)) {
                    //TODO: incomplete numFmt parse, what to do?
                    continue;
                }

                const std::string format = unescape(static_cast<const StringParser&>(numFmt.getAttribute(1)).getValue());
                //TODO: this should probably be done in a better way
                for (size_t i = 0; i < format.length(); ++i) {
                    const char c = format.at(i);
                    if (c == 'd' || c == 'D' ||
                        c == 'm' || c == 'M' ||
                        c == 'y' || c == 'Y' ||
                        c == 'h' || c == 'H' ||
                        c == 's' || c == 'S') {
                        const unsigned long id = static_cast<const IndexParser&>(numFmt.getAttribute(0)).getValue();
                        customDateFormats.insert(id);
                        break;
                    }
                }
            }
        }
    }

    // Process xf mapping & custom date formats
    std::set<unsigned long> dateStyles;
    for (auto const& mapping : xfMapping) {
        if ((mapping.second >= 14 && mapping.second <= 22) ||
            (mapping.second >= 27 && mapping.second <= 36) ||
            (mapping.second >= 45 && mapping.second <= 47) ||
            (mapping.second >= 50 && mapping.second <= 58) ||
            (mapping.second >= 71 && mapping.second <= 81) ||
            (customDateFormats.count(mapping.second) > 0)) {
            dateStyles.insert(mapping.first);
        }
    }
    mDateStyles = dateStyles;

    if (!mz_zip_reader_extract_iter_free(state)) {
        throw std::runtime_error("Reader state was freed but decompression was not finished (styles)");
    }
}

void XlsxFile::parseSharedStrings() {
    if (mPathSharedStrings == "") {
        // No shared strings, not necessarily an error
        mParallelStrings = false;
        return;
    }
    if (mParallelStrings) {
        mFileSharedStrings = new mz_zip_archive;
        memset(mFileSharedStrings, 0, sizeof(*mFileSharedStrings));
        mz_bool status = mz_zip_reader_init_file(mFileSharedStrings, mArchivePath.c_str(), 0);
        if (!status) {
            // failed to initialize secondary file for shared strings,
            // so we fall back to sequential parsing
            delete mFileSharedStrings;
            mFileSharedStrings = nullptr;
            mParallelStrings = false;
        }
    }
    if (mParallelStrings) {
        mParallelStringFuture = std::async(std::launch::async, &XlsxFile::parseSharedStringsInterleaved, this);
    } else {
        parseSharedStringsInterleaved();
    }
}

void XlsxFile::finalize() {
    if (mParallelStrings) {
        // join string thread or resolve exception
        mParallelStringFuture.get();
    }
}

bool XlsxFile::isDate(unsigned long style) const {
    return mDateStyles.count(style) > 0;
}

double XlsxFile::toDate(double date) const {
    // unix time (used by R) is from 1970, excel is from 1900 or 1904
    const double offset = mDate1904 ? 24107 : 25569;
    // excel stores dates as days from 1900/1904, so we need to convert days to seconds
    // also just in case adjust for 1900 leap year bug
    if (!mDate1904 && date < 61) date = date + 1;
    return (date * 86400) - (offset * 86400);;
}

void XlsxFile::prepareDynamicStrings(const int numThreads) {
# if defined(TARGET_R)
    mDynamicStrings.resize(numThreads);
#else
    mDynamicStrings.resize(numThreads);
#endif
}

unsigned long long XlsxFile::addDynamicString(const int threadId, const char* str) {
//#if defined(TARGET_R)
//    const unsigned long idx = mDynamicStrings[threadId].size();
//    mDynamicStrings[threadId].push_back(str);
//#elif defined(TARGET_PYTHON)
    //TODO:
//#else
    // insert threadId as 16 most-significant bits in returned string index
    const unsigned long long baseIndex = mDynamicStrings[threadId].size();
    mDynamicStrings[threadId].push_back(str);
    const unsigned long long idx = baseIndex | ((static_cast<unsigned long long>(threadId) & 0xFFull) << 56);
//#endif
    return idx;
}

const std::string& XlsxFile::getDynamicString(const int threadId, const unsigned long long index) const {
//#if defined(TARGET_R)
//    return mDynamicStrings[threadId][index];
//#else
    //if (threadId >= 0) return mDynamicStrings[threadId][index & 0xFFFFFFFFFFFFFFull];
    // decode embedded threadId
    return mDynamicStrings[index >> 56][index & 0xFFFFFFFFFFFFFFull];
//#endif
}

XlsxSheet XlsxFile::getSheet(const int id) {
    if (id > 0 && id <= static_cast<int>(mSheetIndex.size())) {
        const int archiveIndex = fileIndex(mFile, std::get<3>(mSheetIndex[id - 1]).c_str());
        if (archiveIndex != -1) return XlsxSheet(*this, mFile, archiveIndex);
    }
    throw std::runtime_error("Unable to find specified sheet");
}

XlsxSheet XlsxFile::getSheet(const std::string& name) {
    for (size_t i = 0; i < mSheetIndex.size(); ++i) {
        if (std::get<1>(mSheetIndex[i]) == name) {
            const int archiveIndex = fileIndex(mFile, std::get<3>(mSheetIndex[i]).c_str());
            if (archiveIndex == -1) break;
            return XlsxSheet(*this, mFile, archiveIndex);
        }
    }
    throw std::runtime_error("Unable to find specified sheet");
}

void XlsxFile::parseSharedStringsInterleaved() {
    mz_zip_archive* file = mFileSharedStrings == nullptr ? mFile : mFileSharedStrings;
    const int relIndex = fileIndex(file, mPathSharedStrings.c_str());
    if (relIndex < 0) {
        stringCount.store(-1);
        throw std::runtime_error("Failed to retrieve shared strings file");
    }

    mz_zip_reader_extract_iter_state* state = mz_zip_reader_extract_iter_new(file, relIndex, 0);
    if (!state) {
        stringCount.store(-1);
        throw std::runtime_error("Failed to initialize reader state for shared strings");
    }

    constexpr size_t tBufferSize = 32768;
    char tBuffer[tBufferSize];
    size_t tBufferLength = 0;

    constexpr size_t bufferSize = 32768;
    char buffer[bufferSize];
    size_t offset = 0;
    size_t read = 0;

    ElementParser<1> sst("sst", {"uniqueCount"}, {AttributeType::INDEX});
    ElementParser<0> si("si", {}, {});
    ElementParser<0> t("t", {}, {});

    unsigned long uniqueCount = 0;
    unsigned long numSharedStrings = 0;
    stringCount.store(0);

    while (true) {
        if (offset >= read) {
            mz_zip_error err = MZ_ZIP_NO_ERROR;
            read = mz_zip_reader_extract_iter_read(state, buffer, bufferSize, err);
            if (state->status < 0) {
                mz_zip_reader_extract_iter_free(state);
                stringCount.store(-1);
                throw std::runtime_error("Error while decompressing shared strings");
            }
            if (read == 0) break;
            offset = 0;
        }
        const unsigned char current = buffer[offset];
        ++offset;

        sst.process(current);
        if (!sst.inside()) continue;
        if (sst.completedStart()) {
            if (sst.hasValue(0)) {
                uniqueCount = static_cast<const IndexParser&>(sst.getAttribute(0)).getValue();
#if defined(TARGET_R)
                mSharedStrings = Rcpp::CharacterVector(uniqueCount);
#elif defined(TARGET_PYTHON)
                mSharedStrings.reserve(uniqueCount);
#else
                mSharedStrings.reserve(uniqueCount);
#endif
            }
        }
        bool in_si = si.inside();
        si.process(current);
        if (!in_si) continue;
        bool in_t = t.inside();
        t.process(current);
        if (!in_t && t.inside()) continue;

        if (t.completed()) {
            tBufferLength -= (t.getCloseLength() - 1);
            tBuffer[tBufferLength] = 0;
        }
        if (si.completed()) {
            if (uniqueCount > 0 && numSharedStrings >= uniqueCount) {
                mz_zip_reader_extract_iter_free(state);
                stringCount.store(-1);
                throw std::runtime_error("Parsed more strings than allocated for");
            }
#if defined(TARGET_R)
            if (uniqueCount == 0) {
                // not pre-allocated, so dynamic resizing
                if (numSharedStrings >= static_cast<unsigned long>(mSharedStrings.size())) {
                    Rcpp::CharacterVector newStrings = Rcpp::CharacterVector(mSharedStrings.size() + (mSharedStrings.size() > 1 ? mSharedStrings.size() / 2 : 1));
                    for (int i = 0; i < mSharedStrings.size(); ++i) {
                        newStrings[i] = mSharedStrings[i];
                    }
                    mSharedStrings = newStrings;
                }
            }
            unescape(tBuffer, tBufferLength);
            mSharedStrings[numSharedStrings++] = Rf_mkCharCE(tBuffer, CE_UTF8);
#elif defined(TARGET_PYTHON)
            unescape(tBuffer);
            mSharedStrings.push_back(PyUnicode_FromString(tBuffer));
            numSharedStrings++;
#else
            unescape(tBuffer, tBufferLength);
            mSharedStrings.push_back(tBuffer);
            numSharedStrings = mSharedStrings.size();
#endif
            stringCount.fetch_add(1);
            tBufferLength = 0;
            tBuffer[0] = 0;
            continue;
        }
        if (t.inside()) {
            if (tBufferLength >= tBufferSize) {
                stringCount.store(-1);
                throw std::runtime_error("String exceeded allowed size");
            } else {
                tBuffer[tBufferLength++] = current;
            }
        }
    }
    stringCount.store(-1);

    if (uniqueCount > 0 && numSharedStrings != uniqueCount) {
        throw std::runtime_error("Mismatch between expected and parsed strings (" + std::to_string(uniqueCount) + " vs " + std::to_string(numSharedStrings) + ")");
    }

    if (!mz_zip_reader_extract_iter_free(state)) {
        throw std::runtime_error("Reader state was freed but decompression was not finished (shared strings)");
    }
}

enum {
    MZ_ZIP_LOCAL_DIR_HEADER_SIG = 0x04034b50,
    MZ_ZIP_LOCAL_DIR_HEADER_SIZE = 30,
    MZ_ZIP_CENTRAL_DIR_HEADER_SIZE = 46,

    MZ_ZIP_CDH_COMPRESSED_SIZE_OFS = 20,
    MZ_ZIP_CDH_DECOMPRESSED_SIZE_OFS = 24,

    MZ_ZIP_LDH_FILENAME_LEN_OFS = 26,
    MZ_ZIP_LDH_EXTRA_LEN_OFS = 28
};

bool XlsxFile::getFile(const int fileIndex, size_t& fileOffset, size_t& compSize, size_t& uncompSize) const {
    // large parts copied from miniz
    if (fileIndex < 0) return false;
    mz_zip_archive_file_stat fileStat;
    mz_zip_reader_file_stat(mFile, fileIndex, &fileStat);

    if (!mFile || !mFile->m_pState || static_cast<unsigned int>(fileIndex) >= mFile->m_total_files) {
        throw std::runtime_error("Invalid file parameters");
    }
    const mz_uint8* p = &static_cast<mz_uint8*>(mFile->m_pState->m_central_dir.m_p)[static_cast<mz_uint32*>(mFile->m_pState->m_central_dir_offsets.m_p)[fileIndex]];
    if (!p) {
        throw std::runtime_error("Unable to find file pointer");
    }
    compSize = fileStat.m_comp_size;
    uncompSize = fileStat.m_uncomp_size;

    size_t curFileOfs = fileStat.m_local_header_ofs;
    mz_uint32 local_header_u32[(MZ_ZIP_LOCAL_DIR_HEADER_SIZE + sizeof(mz_uint32) - 1) / sizeof(mz_uint32)];
    mz_uint8 *pLocal_header = (mz_uint8 *)local_header_u32;
    if (mFile->m_pRead(mFile->m_pIO_opaque, curFileOfs, pLocal_header, MZ_ZIP_LOCAL_DIR_HEADER_SIZE) != MZ_ZIP_LOCAL_DIR_HEADER_SIZE) {
        throw std::runtime_error("File read failed");
    }

    if (MZ_READ_LE32(pLocal_header) != MZ_ZIP_LOCAL_DIR_HEADER_SIG) {
        throw std::runtime_error("Invalid header or corrupted");
    }

    curFileOfs += MZ_ZIP_LOCAL_DIR_HEADER_SIZE + MZ_READ_LE16(pLocal_header + MZ_ZIP_LDH_FILENAME_LEN_OFS) + MZ_READ_LE16(pLocal_header + MZ_ZIP_LDH_EXTRA_LEN_OFS);
    if ((curFileOfs + fileStat.m_comp_size) > mFile->m_archive_size) {
        throw std::runtime_error("Invalid header or corrupted");
    }

    fileOffset = curFileOfs;

    return true;
}

STRING_TYPE XlsxFile::getString(const long long index) const {
    if (index < 0 || index >= mSharedStrings.size()) {
        throw std::runtime_error("String index out of bounds");
    }
    while (stringCount.load() <= index && stringCount.load() >= 0) continue; // for if strings are loaded parallel & user-specified columns by headers
    return mSharedStrings[index];
}

void XlsxFile::unescape(char* buffer, const size_t buffer_size) const {
    size_t replaced = 0;
    size_t i = 0;
    while (buffer[i] != '\0' && i < buffer_size) {
        if (buffer[i] == '&') {
            if (i+4 < buffer_size && strncmp(&buffer[i+1], "amp;", 4) == 0) {
                buffer[i-replaced] = '&';
                replaced += 4;
                i += 4;
            } else if (i+5 < buffer_size && strncmp(&buffer[i+1], "apos;", 5) == 0) {
                buffer[i-replaced] = '\'';
                replaced += 5;
                i += 5;
            } else if (i+5 < buffer_size && strncmp(&buffer[i+1], "quot;", 5) == 0) {
                buffer[i-replaced] = '"';
                replaced += 5;
                i += 5;
            } else if (i+3 < buffer_size && strncmp(&buffer[i+1], "gt;", 3) == 0) {
                buffer[i-replaced] = '>';
                replaced += 3;
                i += 3;
            } else if (i+3 < buffer_size && strncmp(&buffer[i+1], "lt;", 3) == 0) {
                buffer[i-replaced] = '<';
                replaced += 3;
                i += 3;
            } else if (i+3 < buffer_size && buffer[i+1] == '#') {
                // numeric character reference
                bool hex = buffer[i+2] == 'x';
                size_t j = 2 + hex;
                size_t num = 0;
                // convert escape string to codepoint
                while (i+j < buffer_size && buffer[i+j] != '\0') {
                    if (buffer[i+j] == ';') break;
                    if (hex) {
                        if ((buffer[i+j] >= '0' && buffer[i+j] <= '9')) {
                            num = (num * 16) + (buffer[i+j] - '0');
                        } else if (buffer[i+j] >= 'A' && buffer[i+j] <= 'F') {
                            num = (num * 16) + 10 + (buffer[i+j] - 'A');
                        } else if (buffer[i+j] >= 'a' && buffer[i+j] <= 'f') {
                            num = (num * 16) + 10 + (buffer[i+j] - 'a');
                        }
                    } else {
                        num = (num * 10) + (buffer[i+j] - '0');
                    }
                    j++;
                }
                //std::cout << "Numeric character reference: " << num << std::endl;
                // convert codepoint to utf-8
                if (num < 0x80) {
                    buffer[i-replaced] = static_cast<unsigned char>(num);
                } else if (num < 0x800) {
                    buffer[i-replaced] = static_cast<unsigned char>(num >> 6) | 0xc0;
                    buffer[i-replaced+1] = static_cast<unsigned char>(num & 0x3f) | 0x80;
                } else if (num < 0x10000) {
                    buffer[i-replaced] = static_cast<unsigned char>(num >> 12) | 0xe0;
                    buffer[i-replaced+1] = static_cast<unsigned char>((num >> 6) & 0x3f) | 0x80;
                    buffer[i-replaced+2] = static_cast<unsigned char>(num & 0x3f) | 0x80;
                    //std::cout << "3 bytes: " << static_cast<unsigned int>(static_cast<unsigned char>(num >> 12) | 0xe0) << " " << static_cast<unsigned int>(static_cast<unsigned char>((num >> 6) & 0x3f) | 0x80) << " " << static_cast<unsigned int>(static_cast<unsigned char>(num & 0x3f) | 0x80) << std::endl;
                } else {
                    buffer[i-replaced] = static_cast<unsigned char>(num >> 18) | 0xf0;
                    buffer[i-replaced+1] = static_cast<unsigned char>((num >> 12) & 0x3f) | 0x80;
                    buffer[i-replaced+2] = static_cast<unsigned char>((num >> 6) & 0x3f) | 0x80;
                    buffer[i-replaced+3] = static_cast<unsigned char>(num & 0x3f) | 0x80;
                }
                replaced += j - (num >= 0x80) - (num >= 0x800) - (num >= 0x10000);
                i += j;
            }
        } else {
            buffer[i-replaced] = buffer[i];
        }
        ++i;
    }
    buffer[i - replaced] = '\0';
}

std::string XlsxFile::unescape(const std::string& string) const {
    char staticBuf[256]{};
    if (string.length() < 256) {
        string.copy(staticBuf, 256);
        unescape(staticBuf, 256);
        return std::string(staticBuf);
    } else {
        char* dynamicBuf = new char[string.length() + 1];
        string.copy(dynamicBuf, string.length());
        unescape(dynamicBuf, string.length() + 1);
        const std::string unescaped(dynamicBuf);
        delete[] dynamicBuf;
        return unescaped;
    }
}
