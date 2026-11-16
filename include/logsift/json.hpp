// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// JSON reading (one object per line) and writing.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "logsift/record.hpp"

namespace logsift {

/// Maximum nesting depth accepted by the JSON parser.
inline constexpr int kMaxJsonDepth = 64;

/// Parses a JSON object into a record. Nested objects are flattened with
/// dotted names ({"http":{"status":200}} gives "http.status"); arrays are
/// kept as compact JSON text in a string field. Integers that fit in 64 bits
/// become Int, other numbers Float. Returns nullopt if the text is not a
/// single JSON object; `error`, when given, receives a description.
std::optional<Record> parse_json_object(std::string_view text, std::string* error = nullptr);

/// JSON text for a value: times as RFC 3339 strings, non-finite floats as null.
std::string value_to_json(const Value& v);

/// One-line JSON object for a record. Internal fields (names starting with
/// '_') are written only when include_internal is set; _time is always
/// written when present.
std::string record_to_json(const Record& r, bool include_internal = false);

}  // namespace logsift
