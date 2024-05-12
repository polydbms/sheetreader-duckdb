#pragma once

#include <array>
#include <vector>
#include <memory>

class ValueParser {
public:
    virtual ~ValueParser() {};
    virtual void process(const unsigned char character) = 0;
    virtual void reset() = 0;
};

class IndexParser : public ValueParser {
    unsigned long mValue;
public:
    void process(const unsigned char character) override {
        mValue = mValue * 10 + (character - '0');
    }

    unsigned long getValue() const {
        return mValue;
    }

    void reset() override {
        mValue = 0;
    }
};

class StringParser : public ValueParser {
    const static size_t sStaticSize = 256;
    unsigned char mStatic[sStaticSize];
    size_t mWrite;
    //TODO: additional dynamic storage
public:
    void process(const unsigned char character) override {
        if (mWrite <= sStaticSize) {
            mStatic[mWrite++] = character;
        } else {
            //TODO: dynamic
        }
    }

    std::string getValue() const {
        if (mWrite <= sStaticSize) {
            return std::string(reinterpret_cast<const char*>(mStatic), mWrite);
        } else {
            //TODO: dynamic
            return std::string("");
        }
    }

    void reset() override {
        mWrite = 0;
        mStatic[0] = 0;
        //TODO: dynamic
    }
};

class LocationParser : public ValueParser {
    unsigned long mColumn;
    unsigned long mRow;
public:
    void process(const unsigned char character) override {
        //TODO: prevent reading digits before alphabetic and alphabetic after digits?
        if (isalpha(character)) {
            mColumn = mColumn * 26 + (character - 64);
        } else {
            mRow = mRow * 10 + (character - '0');
        }
    }

    std::pair<unsigned long, unsigned long> getValue() const {
        return std::pair<unsigned long, unsigned long>(mColumn, mRow);
    }

    void reset() override {
        mColumn = 0;
        mRow = 0;
    }
};

class RangeParser : public ValueParser {
    LocationParser mStart;
    LocationParser mEnd;
    bool mIsEnd;
public:

    void process(const unsigned char character) override {
        if (character == ':') {
            mIsEnd = true;
        } else if (mIsEnd) {
            mEnd.process(character);
        } else {
            mStart.process(character);
        }
    }

    std::pair<std::pair<unsigned long, unsigned long>, std::pair<unsigned long, unsigned long>> getValue() const {
        return std::pair<std::pair<unsigned long, unsigned long>, std::pair<unsigned long, unsigned long>>(mStart.getValue(), mEnd.getValue());
    }

    void reset() override {
        mStart.reset();
        mEnd.reset();
        mIsEnd = false;
    }
};

class TypeParser : public ValueParser {
    CellType mType;
public:
    void process(const unsigned char character) override {
        if (mType == CellType::T_NONE) {
            if (character == 'b') {
                mType = CellType::T_BOOLEAN;
            } else if (character == 'd') {
                mType = CellType::T_DATE;
            } else if (character == 'e') {
                mType = CellType::T_ERROR;
            } else if (character == 'n') {
                mType = CellType::T_NUMERIC;
            } else if (character == 's') {
                mType = CellType::T_STRING_REF;
            } else if (character == 'i') {
                mType = CellType::T_STRING_INLINE;
            }
        } else if (mType == CellType::T_STRING_REF && character == 't') {
            mType = CellType::T_STRING;
        }
    }

    CellType getValue() const {
        return mType;
    }

    void reset() override {
        mType = CellType::T_NONE;
    }
};

enum class AttributeType {
    INDEX,
    STRING,
    LOCATION,
    RANGE,
    TYPE
};

template<size_t N>
class ElementParser {
    const std::string mName;
    int mScan;
    const std::array<std::string, N> mAttributeNames;
    std::array<std::unique_ptr<ValueParser>, N> mAttributeValues;
    std::array<int, N> mAttributeScan;
    std::array<bool, N> mAttributeFlags;
    int mCurrentAttribute;
    bool mPrevCloseSlash;
    int mCloseLength;

    enum class State {
        OUTSIDE,
        START,
        START_NAME,
        START_ATTRIBUTE_NAME,
        START_ATTRIBUTE_VALUE,
        INSIDE,
        END,
        END_NAME
    };

    State mState;
    int mCompleted;

public:
    ElementParser(const std::string& name, std::array<std::string, N> attributes, std::array<AttributeType, N> types)
        : mName(name)
        , mScan(-1)
        , mAttributeNames(attributes)
        , mCurrentAttribute(-1)
        , mPrevCloseSlash(false)
        , mCloseLength(0)
        , mState(State::OUTSIDE)
        , mCompleted(0)
    {
        for (size_t i = 0; i < N; ++i) {
            if (types[i] == AttributeType::INDEX) {
                mAttributeValues[i] = std::unique_ptr<IndexParser>(new IndexParser());
            } else if (types[i] == AttributeType::STRING) {
                mAttributeValues[i] = std::unique_ptr<StringParser>(new StringParser());
            } else if (types[i] == AttributeType::LOCATION) {
                mAttributeValues[i] = std::unique_ptr<LocationParser>(new LocationParser());
            } else if (types[i] == AttributeType::RANGE) {
                mAttributeValues[i] = std::unique_ptr<RangeParser>(new RangeParser());
            } else if (types[i] == AttributeType::TYPE) {
                mAttributeValues[i] = std::unique_ptr<TypeParser>(new TypeParser());
            }
        }
    }

    void process(const unsigned char character) {
        if (mState == State::OUTSIDE) {
            if (character == '<') mState = State::START;
            return;
        }
        const bool whitespace = (character == ' ' || character == '\t' || character == '\n' || character == '\r');
        if (mState == State::START) {
            // skip potential whitespace before name (not allowed by XML spec but just in case)
            if (whitespace) return;
            mPrevCloseSlash = false;
            mState = State::START_NAME;
            mScan = 0;
        }
        if (mState == State::START_NAME) {
            // verify tag name (return to OUTSIDE if not matched)
            if (character == '>' || character == '/' || whitespace) {
                if (mScan == static_cast<int>(mName.length())) {
                    mCompleted = 0;
                    if (character == '>') {
                        if (mPrevCloseSlash) {
                            mCompleted = 2;
                            mCloseLength = 0;
                            mState = State::OUTSIDE;
                        } else {
                            mState = State::INSIDE;
                        }
                    } else if (character == '/') {
                        mPrevCloseSlash = true;
                    } else {
                        mState = State::START_ATTRIBUTE_NAME;
                    }
                    // reset attribute flags
                    for (size_t i = 0; i < N; ++i) mAttributeFlags[i] = false;
                    for (size_t i = 0; i < N; ++i) mAttributeScan[i] = 0;
                    for (size_t i = 0; i < N; ++i) mAttributeValues[i]->reset();
                } else {
                    mState = State::OUTSIDE;
                }
                return;
            }
            if (character == ':') {
                mScan = 0;
                return;
            }
            if (mScan < 0) return;
            if (mScan < static_cast<int>(mName.length())) {
                if (character == mName[mScan]) {
                    ++mScan;
                } else {
                    mScan = -1;
                }
                return;
            } else {
                // not '>' or whitespace after reaching name end
                mScan = -1;
            }
            return;
        }
        if (mState == State::START_ATTRIBUTE_NAME) {
            if (character == '>') {
                if (mPrevCloseSlash) {
                    mCompleted = 2;
                    mCloseLength = 0;
                    mState = State::OUTSIDE;
                } else {
                    mCompleted = 1;
                    mState = State::INSIDE;
                }
            }
            mPrevCloseSlash = false;
            if (character == '/') mPrevCloseSlash = true;
            if constexpr (N == 0) return;
            // process attributes
            if (whitespace) {
                // skip whitespace between attributes (also reset scans)
                bool none = true;
                for (size_t i = 0; i < N; ++i) {
                    if (mAttributeScan[i] > 0) none = false;
                    if (!mAttributeFlags[i]) mAttributeScan[i] = 0;
                }
                if (none) return;
            }
            if (character == ':') {
                // reset scan due to namespace
                for (size_t i = 0; i < N; ++i) mAttributeScan[i] = 0;
                return;
            }
            // scan for each attribute
            for (size_t i = 0; i < N; ++i) {
                // skip if there was already a mismatch or we have already fully processed the attribute
                if (mAttributeScan[i] < 0 || mAttributeFlags[i]) continue;
                // test if the attribute finished scanning successfully
                if (mAttributeScan[i] == static_cast<int>(mAttributeNames[i].length()) && (character == '=' || whitespace)) {
                    mCurrentAttribute = i;
                    for (size_t j = 0; j < N; ++j) mAttributeScan[j] = 0;
                    mState = State::START_ATTRIBUTE_VALUE;
                    break;
                }
                // otherwise advance or disable scan
                if (character == mAttributeNames[i][mAttributeScan[i]]) {
                    ++mAttributeScan[i];
                } else {
                    mAttributeScan[i] = -1;
                }
            }
            return;
        }
        if (mState == State::START_ATTRIBUTE_VALUE) {
            if (mAttributeScan[mCurrentAttribute] == 0) {
                if (character == '"') mAttributeScan[mCurrentAttribute] = 1;
                return;
            } else if (mAttributeScan[mCurrentAttribute] == 1) {
                // actually read/parse value (via individual classes, i.e. IntegerParser, LocationParser, etc.)
                if (character == '"') {
                    mAttributeFlags[mCurrentAttribute] = true;
                    mAttributeScan[mCurrentAttribute] = 0;
                    mCurrentAttribute = -1;
                    mState = State::START_ATTRIBUTE_NAME;
                    return;
                }
                mAttributeValues[mCurrentAttribute]->process(character);
            }
            return;
        }
        if (mState == State::INSIDE) {
            if (character == '<') {
                mState = State::END;
                mCloseLength = 1;
            }
            return;
        }
        if (mState == State::END) {
            if (character == '/') {
                mState = State::END_NAME;
                mScan = 0;
                ++mCloseLength;
            } else {
                mState = State::INSIDE;
            }
            return;
        }
        if (mState == State::END_NAME) {
            // also verify tag name
            ++mCloseLength;
            if (mScan == 0 && whitespace) return;
            if (character == '>' || whitespace) {
                if (mScan == static_cast<int>(mName.length())) {
                    mCompleted = 2;
                    mState = State::OUTSIDE;
                } else {
                    mState = State::INSIDE;
                }
                return;
            }
            if (character == ':') {
                mScan = 0;
                return;
            }
            if (mScan < 0) return;
            if (mScan < static_cast<int>(mName.length())) {
                if (character == mName[mScan]) {
                    ++mScan;
                } else {
                    mScan = -1;
                }
                return;
            } else {
                mScan = -1;
            }
            return;
        }
    }

    bool outside() const {
        return mState == State::OUTSIDE;
    }

    bool inside() const {
        return mState == State::INSIDE || mState == State::END || mState == State::END_NAME;
    }

    bool atStart() const {
        return mState == State::START || mState == State::START_NAME || mState == State::START_ATTRIBUTE_NAME || mState == State::START_ATTRIBUTE_VALUE;
    }

    bool completedStart() {
        const bool ret = mCompleted > 0;
        mCompleted = 0;
        return ret;
    }

    bool completed() {
        const bool ret = mCompleted == 2;
        if (ret) mCompleted = 0;
        return ret;
    }

    bool hasValue(const size_t i) {
        return mAttributeFlags[i];
    }

    const ValueParser& getAttribute(const size_t i) const {
        return *mAttributeValues[i];
    }

    int getCloseLength() const {
        return mCloseLength;
    }

    void reset() {
        mScan = -1;
        mCurrentAttribute = -1;
        mPrevCloseSlash = false;
        mCloseLength = 0;
        mState = State::OUTSIDE;
        mCompleted = 0;
    }
};
