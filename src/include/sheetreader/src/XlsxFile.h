#pragma once

#include <string>
#include <vector>
#include <future>
#include <atomic>
#include <set>

#include "miniz/miniz.h"
#if defined(TARGET_R)
#include <Rcpp.h>
#elif defined(TARGET_PYTHON)
#include <Python.h>
#else
#include <iostream>
#endif

#include "XlsxSheet.h"

class XlsxFile {
public:
    std::string mArchivePath;
    mz_zip_archive* mFile;
    mz_zip_archive* mFileSharedStrings; // separate for shared strings to avoid conflicts with parallel parsing

    std::string mPathWorkbook;
    std::string mPathSharedStrings;
    std::string mPathStyles;

    // sheetId, name, id, target
    std::vector<std::tuple<int, std::string, std::string, std::string>> mSheetIndex;
    bool mDate1904;
    bool mParallelStrings;
    std::future<void> mParallelStringFuture;

    std::atomic_llong stringCount;
#if defined(TARGET_R)
    Rcpp::CharacterVector mSharedStrings;
    std::vector<std::vector<std::string>> mDynamicStrings;
#   define STRING_TYPE SEXP
#elif defined(TARGET_PYTHON)
    //TODO
    std::vector<PyObject*> mSharedStrings;
    std::vector<std::vector<std::string>> mDynamicStrings;
#   define STRING_TYPE PyObject*
#else
    std::vector<std::string> mSharedStrings;
    std::vector<std::vector<std::string>> mDynamicStrings;
#   define STRING_TYPE std::string
#endif
    std::set<unsigned long> mDateStyles;

    XlsxFile(const std::string archivePath);
    ~XlsxFile();
    void parseRootRelationships();
    void parseWorkbook();
    void parseWorkbookRelationships();
    void parseStyles();
    void parseSharedStrings();
    void parseSharedStringsInterleaved();
    void finalize();
    bool isDate(unsigned long style) const;
    double toDate(double date) const;
    void prepareDynamicStrings(const int numThreads);
    unsigned long long addDynamicString(const int threadId, const char* str);
    const std::string& getDynamicString(const int threadId, const unsigned long long index) const;

    int getArchiveIndex(const std::string& path);
    XlsxSheet getSheet(const int id);
    XlsxSheet getSheet(const std::string& name);

    bool getFile(int fileIndex, size_t& fileOffset, size_t& compSize, size_t& uncompSize) const;

    bool isDate(const int style) const;
    STRING_TYPE getString(const long long index) const;
    void unescape(char* buffer, const size_t buffer_size) const;
    std::string unescape(const std::string& string) const;
};
