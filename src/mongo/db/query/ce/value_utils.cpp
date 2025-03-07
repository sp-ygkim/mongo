/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/ce/value_utils.h"

#include "mongo/db/query/ce/max_diff.h"
#include "mongo/db/query/ce/scalar_histogram.h"

namespace mongo::ce {

using namespace sbe;

SBEValue::SBEValue(value::TypeTags tag, value::Value val) : _tag(tag), _val(val) {}

SBEValue::SBEValue(std::pair<value::TypeTags, value::Value> v) : SBEValue(v.first, v.second) {}

SBEValue::SBEValue(const SBEValue& other) {
    auto [tag, val] = copyValue(other._tag, other._val);
    _tag = tag;
    _val = val;
}

SBEValue::SBEValue(SBEValue&& other) {
    _tag = other._tag;
    _val = other._val;

    other._tag = value::TypeTags::Nothing;
    other._val = 0;
}

SBEValue::~SBEValue() {
    value::releaseValue(_tag, _val);
}

SBEValue& SBEValue::operator=(const SBEValue& other) {
    value::releaseValue(_tag, _val);

    auto [tag, val] = copyValue(other._tag, other._val);
    _tag = tag;
    _val = val;
    return *this;
}

SBEValue& SBEValue::operator=(SBEValue&& other) {
    value::releaseValue(_tag, _val);

    _tag = other._tag;
    _val = other._val;

    other._tag = value::TypeTags::Nothing;
    other._val = 0;

    return *this;
}

std::pair<value::TypeTags, value::Value> SBEValue::get() const {
    return std::make_pair(_tag, _val);
}

value::TypeTags SBEValue::getTag() const {
    return _tag;
}

value::Value SBEValue::getValue() const {
    return _val;
}

std::pair<value::TypeTags, value::Value> makeInt64Value(int v) {
    return std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(v));
};

std::pair<value::TypeTags, value::Value> makeNullValue() {
    return std::make_pair(value::TypeTags::Null, 0);
};

bool sameTypeClass(value::TypeTags tag1, value::TypeTags tag2) {
    if (tag1 == tag2) {
        return true;
    }

    static constexpr const char* kTempFieldName = "temp";

    BSONObjBuilder minb1;
    minb1.appendMinForType(kTempFieldName, value::tagToType(tag1));
    const BSONObj min1 = minb1.obj();

    BSONObjBuilder minb2;
    minb2.appendMinForType(kTempFieldName, value::tagToType(tag2));
    const BSONObj min2 = minb2.obj();

    return min1.woCompare(min2) == 0;
}

bool sameTypeBracket(value::TypeTags tag1, value::TypeTags tag2) {
    if (tag1 == tag2) {
        return true;
    }
    return ((value::isNumber(tag1) && value::isNumber(tag2)) ||
            (value::isString(tag1) && value::isString(tag2)));
}

int32_t compareValues(value::TypeTags tag1,
                      value::Value val1,
                      value::TypeTags tag2,
                      value::Value val2) {
    const auto [compareTag, compareVal] = value::compareValue(tag1, val1, tag2, val2);
    uassert(6660547, "Invalid comparison result", compareTag == value::TypeTags::NumberInt32);
    return value::bitcastTo<int32_t>(compareVal);
}

void sortValueVector(std::vector<SBEValue>& sortVector) {
    const auto cmp = [](const SBEValue& a, const SBEValue& b) {
        return compareValues(a.getTag(), a.getValue(), b.getTag(), b.getValue()) < 0;
    };
    std::sort(sortVector.begin(), sortVector.end(), cmp);
}

double valueToDouble(value::TypeTags tag, value::Value val) {
    double result = 0;
    if (value::isNumber(tag)) {
        result = value::numericCast<double>(tag, val);
    } else if (value::isString(tag)) {
        const StringData sd = value::getStringView(tag, val);

        // Convert a prefix of the string to a double.
        const size_t maxPrecision = std::min(sd.size(), sizeof(double));
        for (size_t i = 0; i < maxPrecision; ++i) {
            const char ch = sd[i];
            const double charToDbl = ch / std::pow(2, i * 8);
            result += charToDbl;
        }
    } else if (tag == value::TypeTags::Date || tag == value::TypeTags::Timestamp) {
        int64_t v = value::bitcastTo<int64_t>(val);
        result = value::numericCast<double>(value::TypeTags::NumberInt64, v);

    } else if (tag == value::TypeTags::ObjectId) {
        auto objView =
            ConstDataView(reinterpret_cast<const char*>(sbe::value::getObjectIdView(val)->data()));
        // Take the first 8 bytes of the ObjectId.
        // ToDo: consider using the entire ObjectId or other parts of it
        // 	 auto v = objView.read<LittleEndian<uint64_t>>(sizeof(uint32_t));
        auto v = objView.read<LittleEndian<uint64_t>>();
        result = value::numericCast<double>(value::TypeTags::NumberInt64, v);
    } else {
        uassert(6844500, "Unexpected value type", false);
    }

    return result;
}

bool canEstimateTypeViaHistogram(value::TypeTags tag) {
    if (sbe::value::isNumber(tag) || value::isString(tag)) {
        return true;
    }

    switch (tag) {
        // Other types that we can/do build histograms on:
        // - Date/time types.
        case value::TypeTags::Date:
        case value::TypeTags::Timestamp:
        // - ObjectId.
        case value::TypeTags::ObjectId:
            return true;

        // Types that can only be estimated via the type-counters.
        case value::TypeTags::Object:
        case value::TypeTags::Array:
        case value::TypeTags::Null:
        case value::TypeTags::Boolean:
            return false;

        // Trying to estimate any other types should result in an error.
        default:
            uasserted(7051100,
                      str::stream()
                          << "Type " << tag << " is not supported by histogram estimation.");
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo::ce
