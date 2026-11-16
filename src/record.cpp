// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "logsift/record.hpp"

#include <algorithm>

namespace logsift {

bool is_internal_field(std::string_view name) noexcept { return !name.empty() && name.front() == '_'; }

Record::Record(std::initializer_list<Field> init) {
    for (const auto& f : init) set(f.name, f.value);
}

const Value* Record::find(std::string_view name) const {
    for (const auto& f : fields_) {
        if (f.name == name) return &f.value;
    }
    return nullptr;
}

Value Record::get(std::string_view name) const {
    const Value* v = find(name);
    return v ? *v : Value{};
}

void Record::set(std::string_view name, Value value) {
    for (auto& f : fields_) {
        if (f.name == name) {
            f.value = std::move(value);
            return;
        }
    }
    fields_.push_back(Field{std::string(name), std::move(value)});
}

bool Record::erase(std::string_view name) {
    const auto it = std::find_if(fields_.begin(), fields_.end(), [&](const Field& f) { return f.name == name; });
    if (it == fields_.end()) return false;
    fields_.erase(it);
    return true;
}

bool Record::rename(std::string_view from, std::string_view to) {
    if (from == to) return has(from);
    const auto it = std::find_if(fields_.begin(), fields_.end(), [&](const Field& f) { return f.name == from; });
    if (it == fields_.end()) return false;
    Value moved = std::move(it->value);
    const auto pos = static_cast<std::size_t>(it - fields_.begin());
    fields_.erase(it);
    erase(to);
    // Keep the renamed field where the original was (adjusting for an
    // earlier field that was removed).
    std::size_t insert_at = std::min(pos, fields_.size());
    fields_.insert(fields_.begin() + static_cast<std::ptrdiff_t>(insert_at), Field{std::string(to), std::move(moved)});
    return true;
}

std::optional<TimePoint> Record::time() const {
    const Value* v = find(fields::kTime);
    if (!v || !v->is_time()) return std::nullopt;
    return v->as_time();
}

}  // namespace logsift
