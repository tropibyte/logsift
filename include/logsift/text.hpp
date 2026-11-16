// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.
//
// Small string helpers.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace logsift {

std::string_view trim(std::string_view s) noexcept;
std::string to_lower(std::string_view s);
std::string to_upper(std::string_view s);
bool iequals(std::string_view a, std::string_view b) noexcept;
bool starts_with(std::string_view s, std::string_view prefix) noexcept;
bool ends_with(std::string_view s, std::string_view suffix) noexcept;

/// Splits on a delimiter, keeping empty pieces.
std::vector<std::string_view> split(std::string_view s, char delimiter);

/// Number of UTF-8 code points (invalid bytes count as one each).
std::size_t display_width(std::string_view s) noexcept;

/// Truncates to at most `width` code points, ending with "…" when cut.
std::string truncate_display(std::string_view s, std::size_t width);

/// Escapes a string for a JSON string literal (without the quotes).
std::string json_escape(std::string_view s);

/// Appends a Unicode code point as UTF-8.
void append_utf8(std::string& out, char32_t cp);

}  // namespace logsift
