#include "XlsxSheet.h"

#include <map>
#include <thread>
#include <atomic>
#include <array>
#include <chrono>

#include "XlsxFile.h"

#include "fast_double_parser/fast_double_parser.h"
#include "parsing.h"

#if defined(TARGET_R)
#define WARN(x) Rcpp::warning(x)
#else
#include <iostream>
#define WARN(x) std::cout << x << std::endl
#endif

unsigned long strtoul(const char* start, const unsigned long length) {
    unsigned long val = 0;
    unsigned long offset = 0;
    while(offset < length) {
        val = val * 10 + start[offset++] - '0';
    }
    return val;
}

unsigned long alphatoul(const char* start, const unsigned long length) {
    unsigned long val = 0;
    unsigned long offset = 0;
    while(offset < length) {
        val = val * 26 + start[offset++] - 64;
    }
    return val;
}

// Extracts the unsigned number from the buffer at the given offset with no checks whatsoever.
// Also assumes the first character is a digit
inline size_t extractUnsigned(const char* buffer, const size_t baseOffset, const size_t bufferSize) {
    size_t offset = 0;
    size_t d;
    size_t n = buffer[baseOffset] - '0';

    while (baseOffset + offset < bufferSize && (d = (buffer[baseOffset + ++offset] - '0')) <= 9) {
        n = n * 10 + d;
    }

    return n;
}

XlsxSheet::XlsxSheet(XlsxSheet&& sheet)
    : mParentFile(sheet.mParentFile)
    , mFile(sheet.mFile)
    , mArchiveIndex(sheet.mArchiveIndex)
    , mHeaders(sheet.mHeaders)
    , mDimension(0, 0)
    , mSpecifiedTypes(false)
{
}

XlsxSheet::XlsxSheet(XlsxFile& parentFile, mz_zip_archive* file, int archiveIndex)
    : mParentFile(parentFile)
    , mFile(file)
    , mArchiveIndex(archiveIndex)
    , mHeaders(false)
    , mDimension(0, 0)
    , mSpecifiedTypes(false)
{
}

void XlsxSheet::specifyTypes(std::vector<CellType> colTypesByIndex, std::map<std::string, CellType> colTypesByName) {
    this->mSpecifiedTypes = true;
    this->mColTypesByIndex = colTypesByIndex;
    this->mColTypesByName = colTypesByName;
}

bool XlsxSheet::interleaved(const int skipRows, const int skipColumns, const int numThreads) {
    constexpr const size_t tbufferSize = BUFFER_SIZE + 1;

    constexpr const size_t numBuffers = 1024;
    std::array<unsigned char*, numBuffers> buffers;
    for (size_t i = 0; i < numBuffers; ++i) {
        buffers[i] = new unsigned char[tbufferSize]; //TODO RAII with unique_ptr?
        memset(buffers[i], 0, tbufferSize);
    }
    mSkipRows = skipRows;
    mSkipColumns = skipColumns;

    std::vector<std::atomic_size_t> readIndexes(numThreads);
    std::atomic_size_t writeIndex(numThreads - 1); // gets incremented at the start to num_threads
    std::atomic_bool terminate(false);
    std::atomic_bool finishedWriting(false);
    std::mutex headerMutex;
    std::atomic_int headerDone(mColTypesByName.size() > 0 ? numThreads : 0); // only have to wait on header if any mColTypesByName

    std::vector<std::thread> parseThreads;
    parseThreads.reserve(numThreads - 1);
    std::vector<ParseState<numBuffers>> parseStates;
    parseStates.reserve(numThreads);

    mCells.resize(numThreads);
    mLocationInfos.resize(numThreads);
    for (int i = 0; i < numThreads; ++i) {
        readIndexes[i].store(static_cast<size_t>(i));
        parseStates.push_back(ParseState<numBuffers>{
            static_cast<size_t>(i),
            buffers,
            tbufferSize,
            writeIndex,
            finishedWriting,
            readIndexes,
            terminate,
            {0, 0},
            headerMutex,
            headerDone
        });
    }
    mParentFile.prepareDynamicStrings(numThreads);

    const int archiveIndex = mArchiveIndex;
    bool success = true;
    auto producerFunc = [&]() {
        mz_zip_reader_extract_iter_state* state = mz_zip_reader_extract_iter_new(mFile, archiveIndex, 0);
        if (!state) {
            throw std::runtime_error("Failed to initialize sheet reader state");
        }

        // shouldn't need to capture numBuffers (constexpr), but MSVC throws error
        const auto checkIndexes = [&](const size_t indexMod, const std::vector<std::atomic_size_t>& readIndexes) {
            for (const auto& index : readIndexes) {
                if (indexMod == (index.load() % numBuffers)) return true;
            }
            return false;
        };

        while (true) {
            if (terminate.load()) return;
            const size_t index = writeIndex.load() + 1;
            while (checkIndexes(index % numBuffers, readIndexes)) {
                if (terminate.load()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if ((!state) || (!state->pZip) || (!state->pZip->m_pState)) {
                finishedWriting.store(true);
                writeIndex.store(index);
                break;
            }

            mz_zip_error err = MZ_ZIP_NO_ERROR;
            const size_t read = mz_zip_reader_extract_iter_read(state, buffers[index % numBuffers], tbufferSize - 1, err);
            if (state->status < 0 || read >= tbufferSize) {
                // decompression error or write error, abort
                writeIndex.store(0);
                finishedWriting.store(true);
                success = false;
                return;
            }
            buffers[index % numBuffers][read] = '\0';
            const bool finished = read == 0 || state->status == TINFL_STATUS_DONE;
            if (finished) finishedWriting.store(true);
            writeIndex.store(index + finished);
            if (finished) break;
        }

        if (state->file_crc32 != state->file_stat.m_crc32) {
            // mismatching crc32
            success = false;
        }
        if (!mz_zip_reader_extract_iter_free(state)) {
            // probably decompression wasn't finished yet
            success = false;
        }
    };
    std::thread producerThread;
    try {
        producerThread = std::thread(producerFunc);
    } catch (std::system_error& e) {
        WARN("Failed to create producer thread");
        for (size_t i = 0; i < numBuffers; ++i) {
            delete[] buffers[i];
        }
        return false;
    }

    for (int i = 0; i < numThreads - 1; ++i) {
        try {
            //parseThreads.push_back(std::thread(&XlsxSheet::interleavedFunc<numBuffers>, this, numThreads, i, std::ref(buffers), tbufferSize, std::cref(writeIndex), std::cref(finishedWriting), std::ref(readIndexes), std::cref(terminate), std::ref(maxDim[i])));
            parseThreads.push_back(std::thread(&XlsxSheet::interleavedFunc<numBuffers>, this, numThreads, std::ref(parseStates[i])));
        } catch (const std::system_error& e) {
            // failed to create thread, kill already created threads (including producer)
            WARN("Failed to create parse thread");
            terminate.store(true);
            for (int j = i - 1; j >= 0; --j) {
                parseThreads[j].join();
            }
            producerThread.join();
            for (size_t i = 0; i < numBuffers; ++i) {
                delete[] buffers[i];
            }
            return false;
        }
    }

    //interleavedFunc<numBuffers>(numThreads, numThreads - 1, buffers, tbufferSize, writeIndex, finishedWriting, readIndexes, terminate, maxDim[numThreads - 1]);
    interleavedFunc<numBuffers>(numThreads, parseStates[numThreads - 1]);
    
    producerThread.join();

    for (size_t i = 0; i < parseThreads.size(); ++i) {
        parseThreads[i].join();
    }

    for (int i = 0; i < numThreads; ++i) {
        if (mDimension.first < parseStates[i].maxCell.first) mDimension.first = parseStates[i].maxCell.first;
        if (mDimension.second < parseStates[i].maxCell.second) mDimension.second = parseStates[i].maxCell.second;
    }

    for (size_t i = 0; i < numBuffers; ++i) {
        delete[] buffers[i];
    }
    if (!success && writeIndex.load() == 0) {
        // something went potentially really wrong during decompression
        throw std::runtime_error("Errors during decompression");
    }
    return success;
}

const char* endp(const char* buf, const size_t bufSize) {
    const char* lastChar = buf;
    for (size_t i = 0; i < bufSize; ++i) {
        if (buf[i] == 0) break;
        if (buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r') lastChar = buf + i;
    }
    return lastChar + 1;
}

template<std::size_t numBuffers>
void XlsxSheet::interleavedFunc(size_t numThreads, ParseState<numBuffers>& parseState) {
    constexpr size_t cellValueBufferSize = BUFFER_SIZE; // Max number of characters in cell is 32767
    char cellValueBuffer[cellValueBufferSize];
    size_t cellValueLength = 0;

    size_t offset = 0;

    std::atomic_size_t& readIndex = parseState.readIndexes[parseState.threadId];
    size_t currentReadBuffer = readIndex.load();

    ElementParser<1> dimension("dimension", {"ref"}, {AttributeType::RANGE});
    //ElementParser<0> sheetData("sheetData", {}, {});
    ElementParser<1> row("row", {"r"}, {AttributeType::INDEX});
    ElementParser<3> c("c", {"r", "t", "s"}, {AttributeType::LOCATION, AttributeType::TYPE, AttributeType::INDEX});
    ElementParser<0> v("v", {}, {});
    ElementParser<0> t("t", {}, {});

    bool loadNext = false;
    bool continueCell = false;
    bool continueRow = false;

    long long expectedRow = -1;
    long long expectedColumn = -1;
    std::list<std::vector<XlsxCell>>& cells = mCells[parseState.threadId];
    std::vector<LocationInfo>& locs = mLocationInfos[parseState.threadId];

    try {
    while (readIndex.load() < parseState.writeIndex.load() || !parseState.finishedWriting.load() || parseState.terminate.load()) {
        if (parseState.terminate.load()) return;
        if (offset >= parseState.bufferSize || parseState.buffers[currentReadBuffer % numBuffers][offset] == '\0' || loadNext) {
            const size_t prevBuffer = currentReadBuffer;
            const bool cellExtension = !c.outside();
            const bool rowExtension = row.atStart();
            if (cellExtension) {
                if (loadNext && continueRow) {
                    // cell extension masked by false row extension (shouldn't happen?)
                    continueRow = false;
                    loadNext = false;
                    continue;
                }
                // cell may not have been fully parsed yet, extend into the next buffer to finish
                currentReadBuffer = readIndex.load() + 1;
                continueCell = true;
                //if (offset < bufferSize - 1) std::cout << "Cell extension " << currentReadBuffer << " at " << offset << std::endl;
            } 
            if (rowExtension) {
                // PROBLEM: there might have been a false cell extension masking a real row extension
                // if unhandled, a new row extension would start at the beginning of the current buffer
                if (loadNext && continueCell) {
                    // row extension masked by false cell extension
                    //std::cout << "Row extension masked by false cell extension " << currentReadBuffer << " at " << offset << std::endl;
                    continueCell = false;
                    loadNext = false;
                    continue;
                } else {
                    // standard row parse extension, avoid double-increment!
                    if (!cellExtension) currentReadBuffer = readIndex.load() + 1;
                    //if (offset < bufferSize - 1) std::cout << "Row extension " << currentReadBuffer << " at " << offset << " (" << continueRow << ")" << std::endl;
                    continueRow = true;
                }
            }
            if (!cellExtension && !rowExtension) {
                // standard leapfrog to next buffer for this thread
                currentReadBuffer = readIndex.load() + numThreads - (continueCell || continueRow);
                continueCell = false;
                continueRow = false;
                cells.emplace_back(std::vector<XlsxCell>());
                cells.back().reserve(800); //TODO: dynamically based on average already parsed?
                expectedRow = -1;
                expectedColumn = -1;
            } 

            // if a parse extension was long enough (unrealistic) or only 1 thread was specified (more likely)
            // we could end up at the same buffer we are already at
            if (currentReadBuffer != prevBuffer) {
                while (!parseState.finishedWriting.load() && currentReadBuffer >= parseState.writeIndex.load()) {
                    if (parseState.terminate.load()) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (parseState.finishedWriting.load() && currentReadBuffer > parseState.writeIndex.load()) {
                    currentReadBuffer = parseState.writeIndex.load();
                }
                if (currentReadBuffer > parseState.writeIndex.load() && parseState.finishedWriting.load()) break;
                readIndex.store(currentReadBuffer);
                if (parseState.buffers[currentReadBuffer % numBuffers][0] == '\0') {
                    continue;
                }
                offset = 0;
            }
            loadNext = false;
        }

        const unsigned char current = parseState.buffers[currentReadBuffer % numBuffers][offset];
        ++offset;

        if (mDimension.first == 0) {
            dimension.process(current);
            if (dimension.completed() && dimension.hasValue(0)) {
                const auto val = static_cast<const RangeParser&>(dimension.getAttribute(0)).getValue();
                //TODO: mSkipColumns & mSkipRows
                mDimension.first = val.second.first;
                mDimension.second = val.second.second;
            }
        }
        bool in_c = c.inside();
        c.process(current);
        if (!in_c) row.process(current); // false row extension could prevent v.process
        if (row.completedStart() && row.inside()) {
            unsigned long r = row.hasValue(0) ? static_cast<const IndexParser&>(row.getAttribute(0)).getValue() - 1 : -1;
            //std::cout << "Row completed: " << r << std::endl;
            locs.push_back(LocationInfo{static_cast<unsigned long>(cells.size()) - 1, static_cast<unsigned long>(cells.back().size()), 0, r});
            if (row.hasValue(0)) {
                //locs.push_back(LocationInfo{cells.size() - 1, cells.back().size(), 0, r});
                expectedRow = r;
            } else {
                //std::cout << "Row with no r attribute " << threadId << ", " << currentReadBuffer << " at " << offset << std::endl;
                //std::cout << buffers[currentReadBuffer % numBuffers] << std::endl;
                expectedRow = -1;
            }
            expectedColumn = 1;
            // we are only interested in the row opening tag, so reset here (also otherwise might interfere with parse extension)
            row.reset();
            if (continueRow) {
                //std::cout << "Ended row extension " << currentReadBuffer << " at " << offset << std::endl;
                loadNext = true;
                continue;
            }
        } else if (continueRow && !row.atStart()) {
            //std::cout << "Ended false row extension " << currentReadBuffer << " at " << offset << std::endl;
            loadNext = true;
            continue;
        }
        if (!in_c && !(continueCell && c.outside())) continue;
        bool in_v = v.inside();
        bool in_t = t.inside();
        v.process(current);
        t.process(current);
        if (!in_v && v.inside()) {
            cellValueLength = 0;
            continue;
        }
        if (!in_t && t.inside()) {
            cellValueLength = 0;
            continue;
        }
        
        if (c.completed()) {
            CellType cellType = CellType::T_NUMERIC;
            bool dateStyle = false;
            if (c.hasValue(0)) {
                const std::pair<unsigned long, unsigned long> val = static_cast<const LocationParser&>(c.getAttribute(0)).getValue();
                if (expectedColumn != static_cast<long long>(val.first) || expectedRow != static_cast<long long>(val.second) - 1) {
                    //std::cout << "Expectation mismatch: " << expectedColumn << " / " << expectedRow << ", " << val.first << " / " << val.second - 1 << std::endl;
                    locs.push_back(LocationInfo{static_cast<unsigned long>(cells.size()) - 1, static_cast<unsigned long>(cells.back().size()), val.first - 1, val.second - 1});
                    expectedColumn = val.first;
                    expectedRow = val.second - 1;
                }
            }
            if (expectedColumn <= static_cast<long long>(mSkipColumns) || expectedRow < static_cast<long long>(mSkipRows)) {
                cellValueLength = 0;
                cellValueBuffer[0] = 0;
                if (expectedColumn != -1) ++expectedColumn;
                if (continueCell) {
                    loadNext = true;
                }
                continue;
            }
            if (c.hasValue(1)) cellType = static_cast<const TypeParser&>(c.getAttribute(1)).getValue();
            if (c.hasValue(2)) dateStyle = mParentFile.isDate(static_cast<const IndexParser&>(c.getAttribute(2)).getValue());

            cellValueLength = cellValueLength - (cellType != CellType::T_STRING_INLINE ? v.getCloseLength() : t.getCloseLength()) + 1;
            if (cellValueLength == 0) {// || (cellType != CellType::T_STRING_INLINE && static_cast<int>(cellValueLength) < v.getCloseLength()) || (cellType == CellType::T_STRING_INLINE && static_cast<int>(cellValueLength) < t.getCloseLength())) {
                // no value
                cellValueLength = 0;
                cellValueBuffer[0] = 0;
                continue;
            }

            cellValueBuffer[cellValueLength + 1] = 0;
            /*if (rowNumber == 0 || cellColumn == 0 || cellType == CellType::T_NONE) {
                throw std::runtime_error("Error when parsing cell");
            }*/
            CellType coerceType = CellType::T_NONE;
            if (mSpecifiedTypes) {
                const long long calcCol = (expectedColumn - static_cast<long long>(mSkipColumns) - 1);
                if (mHeaders && (expectedRow - mSkipRows) == 0) {
                    std::lock_guard<std::mutex>(parseState.headerMutex);
                    //std::cout << "Header " << parseState.threadId << ": " << expectedRow << " / " << expectedColumn << ", " << static_cast<int>(cellType) << ": " << cellValueBuffer << std::endl;
                    if (mColTypesByName.size() > 0) {
                        // 1 by 1 remove from mColTypesByName and insert into mColTypesByIndex
                        std::string name;
                        if (cellType == CellType::T_STRING_REF) {
                            const unsigned long long refIdx = extractUnsigned(cellValueBuffer, 0, cellValueLength);
#if defined(TARGET_R)
                            name = CHAR(mParentFile.getString(refIdx));
#elif defined(TARGET_PYTHON)
                            name = PyUnicode_AsUTF8(mParentFile.getString(refIdx));
#else
                            name = mParentFile.getString(refIdx);
#endif                            
                        } else {
                            name = std::string(cellValueBuffer, cellValueLength);
                        }
                        const auto colType = mColTypesByName.find(name);
                        if (colType != mColTypesByName.end()) {
                            if (calcCol >= static_cast<long long>(mColTypesByIndex.size())) mColTypesByIndex.resize(calcCol + 1);
                            mColTypesByIndex[calcCol] = colType->second;
                            mColTypesByName.erase(colType);
                        }
                    }
                } else {
                    // if mSpecifiedTypes we wait until done with header to continue (headerDone init with 0 if no mColTypesByName)
                    //if (parseState.headerDone.load() > 0) std::cout << "Wait " << parseState.threadId << ": " << expectedRow << " / " << expectedColumn << ", " << parseState.headerDone.load() << " :: " << mHeaders << ", " << (expectedRow - mSkipRows) << std::endl;
                    if (mHeaders && (expectedRow - mSkipRows) > 0 && parseState.headerDone.load() > 0) parseState.headerDone.fetch_sub(1);
                    while (parseState.headerDone.load() > 0) {
                        if (parseState.terminate.load()) return;
                    }
                    if (calcCol < static_cast<long long>(mColTypesByIndex.size())) coerceType = mColTypesByIndex[calcCol];
                }
            }
            const bool explicitNumeric = coerceType == CellType::T_NUMERIC; // to better differentiate date <> numeric
            if (coerceType == CellType::T_NONE) coerceType = cellType;
            //std::cout << "Thread " << parseState.threadId << ": " << expectedRow << " / " << expectedColumn << ", " << static_cast<int>(cellType) << " X " << static_cast<int>(coerceType) << " (offset " << offset << "): " << cellValueBuffer << std::endl;
            XlsxCell cll;
            //TODO: coerceType T_SKIP
            if (coerceType == CellType::T_STRING || coerceType == CellType::T_STRING_INLINE || coerceType == CellType::T_STRING_REF) {
                if (cellType == CellType::T_STRING_REF) {
                    const unsigned long long value = extractUnsigned(cellValueBuffer, 0, cellValueLength);
                    cll.data.integer = value;
                    coerceType = CellType::T_STRING_REF;
                } else {
                    mParentFile.unescape(cellValueBuffer, cellValueLength);
                    const unsigned long long stringIndex = mParentFile.addDynamicString(parseState.threadId, cellValueBuffer);
                    cll.data.integer = stringIndex;
                    coerceType = CellType::T_STRING;
                }
            } else if (coerceType == CellType::T_NUMERIC || coerceType == CellType::T_DATE) {
                double value = 0;
                const char* isok = nullptr;
                if (cellType == CellType::T_STRING_REF) {
                    const unsigned long long refIdx = extractUnsigned(cellValueBuffer, 0, cellValueLength);
#if defined(TARGET_R)
                    const auto str = CHAR(mParentFile.getString(refIdx));
#elif defined(TARGET_PYTHON)
                    const auto str = PyUnicode_AsUTF8(mParentFile.getString(refIdx));
#else
                    const auto str = mParentFile.getString(refIdx).c_str();
#endif
                    isok = fast_double_parser::parse_number(str, &value);
                    //TODO: what about trailing whitespace?
                    //std::cout << "String to number " << parseState.threadId << ": " << expectedRow << " / " << expectedColumn << ", " << static_cast<int>(cellType) << " X " << static_cast<int>(coerceType) << " (offset " << offset << "): " << cellValueBuffer << ", " << str << ", " << value << std::endl;
                    if (isok != endp(str, strlen(str))) isok = nullptr;
                } else if (cellValueLength > 0) {
                    isok = fast_double_parser::parse_number(cellValueBuffer, &value);
                    //std::cout << static_cast<const void*>(isok) << ", " << static_cast<const void*>(endp(cellValueBuffer, cellValueLength)) << " (" << static_cast<const void*>(cellValueBuffer) << ", " << cellValueLength << ", " << cellValueBuffer << ")" << std::endl;
                    if (isok != endp(cellValueBuffer, cellValueLength)) isok = nullptr;
                }
                if (isok == nullptr) {
                    if (cellType == CellType::T_NUMERIC) throw std::runtime_error("Error when parsing number '" + std::string(cellValueBuffer) + "'");
                    // coerceType numeric or date, insert NA
                    coerceType = CellType::T_NONE;
                } else {
                    if ((dateStyle || coerceType == CellType::T_DATE) && !explicitNumeric) {
                        //TODO: coerceType date checking if valid value
                        coerceType = CellType::T_DATE;
                        cll.data.real = mParentFile.toDate(value);
                    } else {
                        //placeCell(value, cellType, rowNumber, cellColumn);
                        cll.data.real = value;
                    }
                }
            } else if (coerceType == CellType::T_BOOLEAN) {
                if (cellType == CellType::T_BOOLEAN) {
                    cll.data.boolean = extractUnsigned(cellValueBuffer, 0, cellValueLength) > 0;
                } else if (cellType == CellType::T_NUMERIC || cellType == CellType::T_DATE) {
                    double value = 0;
                    const char* isok = fast_double_parser::parse_number(cellValueBuffer, &value);
                    cll.data.boolean = (isok != nullptr) ? (value > 0) : false;
                } else if (cellType == CellType::T_STRING || cellType == CellType::T_STRING_INLINE) {
                    cll.data.boolean = (strncmp(cellValueBuffer, "TRUE", 4) == 0);
                } else if (cellType == CellType::T_STRING_REF) {
                    const unsigned long long refIdx = extractUnsigned(cellValueBuffer, 0, cellValueLength);
#if defined(TARGET_R)
                    const auto str = CHAR(mParentFile.getString(refIdx));
#elif defined(TARGET_PYTHON)
                    const auto str = PyUnicode_AsUTF8(mParentFile.getString(refIdx));
#else
                    const auto str = mParentFile.getString(refIdx).c_str();
#endif
                    cll.data.boolean = (strncmp(str, "TRUE", 4) == 0);
                }
            } else if (coerceType == CellType::T_ERROR) {
                //coerceType = CellType::T_NONE;
            } else {
                //std::cout << "Unknown cell/coerce type " << parseState.threadId << ": " << expectedRow << " / " << expectedColumn << ", " << static_cast<int>(cellType) << " X " << static_cast<int>(coerceType) << " (offset " << offset << "): " << cellValueBuffer << std::endl;
            }
            cll.type = coerceType;
            cells.back().push_back(cll);
            cellValueLength = 0;
            cellValueBuffer[0] = 0;
            
            parseState.maxCell.first = expectedColumn;
            parseState.maxCell.second = expectedRow;
            if (expectedColumn != -1) ++expectedColumn;
            if (continueCell) {
                //std::cout << "Ended cell extension " << currentReadBuffer << " at " << offset << std::endl;
                loadNext = true;
            }
            continue;
        } else if (continueCell && c.outside()) {
            // extension into next buffer may have not been needed (not actually an open cell)
            //std::cout << "Ended false cell extension " << currentReadBuffer << " at " << offset << std::endl;
            loadNext = true;
            continue;
        }
        if (v.inside() || t.inside()) {
            if (cellValueLength >= cellValueBufferSize) {
                throw std::runtime_error("Exceeded cell value buffer size");
            }
            cellValueBuffer[cellValueLength++] = current;
        }
    }
    parseState.headerDone.fetch_sub(1); // with a small enough file, a thread might not have done this as intended
    } catch (const std::exception& e) {
        WARN("Parse exception: " + std::string(e.what()));
        parseState.terminate.store(true);
    }
}

std::pair<size_t, std::vector<XlsxCell>> XlsxSheet::nextRow() {
    if (mCells.size() == 0) {
        //std::cout << "Iteration stop 1" << std::endl;
        return std::make_pair(0, std::vector<XlsxCell>());
    }
    if (currentLocs.size() == 0) {
        maxBuffers = mCells[0].size();
        currentBuffer = 0;
        currentThread = 0;
        currentCell = 0;
        currentColumn = 0;
        currentRow = -1;
        currentLocs = std::vector<size_t>(mCells.size(), 0);
    }
    std::vector<XlsxCell> currentValues;
	for (; currentBuffer < maxBuffers; ++currentBuffer) {
		for (; currentThread < mCells.size(); ++currentThread) {
			if (mCells[currentThread].size() == 0) {
                //std::cout << "Iteration stop 2" << std::endl;
                currentBuffer = maxBuffers;
                const auto ret = std::pair<size_t, std::vector<XlsxCell>>(currentRow, currentValues);
                currentRow = 0;
                return ret;
			}
			//std::cout << currentBuffer << " / " << maxBuffers << ", " << currentThread << "/" << mCells.size() << ", " << currentCell << ", " << mCells[currentThread].size() << std::endl;
			const std::vector<XlsxCell> cells = mCells[currentThread].front();
			const std::vector<LocationInfo>& locs = mLocationInfos[currentThread];
			size_t& currentLoc = currentLocs[currentThread];

			// currentCell <= cells.size() because there might be location info after last cell
			for (; currentCell <= cells.size(); ++currentCell) {
				while (currentLoc < locs.size() && locs[currentLoc].buffer == currentBuffer && locs[currentLoc].cell == currentCell) {
					//std::cout << "loc " << currentLoc << "/" << locs.size() << ": " << locs[currentLoc].buffer << " vs " << currentBuffer << ", " << locs[currentLoc].cell << " vs " << currentCell << " (" << locs[currentLoc].column << "/" << locs[currentLoc].row << "), " << currentRow << std::endl;
					currentColumn = locs[currentLoc].column;
					if (locs[currentLoc].row == -1ul) {
						++currentRow;
					    ++currentLoc;
                        //std::cout << "DEBUG1 " << currentRow << std::endl;
                        if (currentRow > 0) return std::pair<size_t, std::vector<XlsxCell>>(currentRow - 1, currentValues);
					} else if (static_cast<long long>(locs[currentLoc].row) > currentRow) {
                        const long long nextRow = static_cast<long long>(locs[currentLoc].row);
                        //std::cout << "nextRow, currentRow: " << nextRow << ", " << currentRow << std::endl;
                        if (nextRow > currentRow + 1) {
                            ++currentRow;
                        } else {
                            currentRow = nextRow;
                            ++currentLoc;
                        }
                        //std::cout << "DEBUG2 " << currentRow << std::endl;
                        if (currentRow > 0) return std::pair<size_t, std::vector<XlsxCell>>(currentRow - 1, currentValues);
					} else {
                        ++currentLoc;
                    }
				}
                //std::cout << "currentCell " << currentCell << " / " << cells.size() << ", " << currentColumn << std::endl;
				if (currentCell >= cells.size()) break;
				const auto adjustedColumn = currentColumn;
                ++currentColumn;
				const XlsxCell& cell = cells[currentCell];
                if (adjustedColumn >= currentValues.size()) currentValues.resize(mDimension.first - mSkipColumns, XlsxCell());
                currentValues[adjustedColumn] = cell;
            }
            mCells[currentThread].pop_front();
            currentCell = 0;
        }
        currentThread = 0;
    }
    const auto ret = std::pair<size_t, std::vector<XlsxCell>>(currentRow, currentValues);
    currentRow = 0;
    return ret;
}
