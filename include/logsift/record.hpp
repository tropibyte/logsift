// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// A log record: an ordered set of named fields.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "logsift/value.hpp"

namespace logsift {

/// Field names with special meaning.
namespace fields {
inline constexpr std::string_view kTime = "_time";  // parsed timestamp (Time)
inline constexpr std::string_view kRaw = "_raw";    // the original line
inline constexpr std::string_view kLine = "_line";  // 1-based line number in its source
inline constexpr std::string_view kFile = "_file";  // source name, when reading files
inline constexpr std::string_view kParseError = "_parse_error";  // line did not match the format
}  // namespace fields

/// True for names starting with '_', which output hides unless asked.
bool is_internal_field(std::string_view name) noexcept;

class Record {
public:
    struct Field {
        std::string name;
        Value value;
        friend bool operator==(const Field&, const Field&) = default;
    };

    Record() = default;
    Record(std::initializer_list<Field> init);

    /// Pointer to a field's value, or nullptr if absent.
    const Value* find(std::string_view name) const;

    /// A field's value, or null if absent.
    Value get(std::string_view name) const;

    bool has(std::string_view name) const { return find(name) != nullptr; }

    /// Sets a field, replacing an existing one in place or appending.
    void set(std::string_view name, Value value);

    /// Removes a field; returns false if it was absent.
    bool erase(std::string_view name);

    /// Renames a field, replacing any field already called `to`. Returns
    /// false if `from` is absent.
    bool rename(std::string_view from, std::string_view to);

    /// The _time field, if it holds a Time.
    std::optional<TimePoint> time() const;

    const std::vector<Field>& fields() const noexcept { return fields_; }
    std::size_t size() const noexcept { return fields_.size(); }
    bool empty() const noexcept { return fields_.empty(); }

    friend bool operator==(const Record&, const Record&) = default;

private:
    std::vector<Field> fields_;
};

}  // namespace logsift
