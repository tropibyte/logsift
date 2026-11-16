// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "logsift/store.hpp"

#include <algorithm>
#include <iterator>

#include "logsift/pipeline.hpp"

namespace logsift {

// ---- LogStore ---------------------------------------------------------------------

namespace {

// _time as a TimePoint; strings and numbers are converted the same way
// compare_query() converts them, so the index agrees with the filter.
std::optional<TimePoint> indexed_time(const Record& r) {
    const Value* v = r.find(fields::kTime);
    if (!v) return std::nullopt;
    return v->to_time();
}

std::vector<std::size_t> intersect(const std::vector<std::size_t>& a, const std::vector<std::size_t>& b) {
    std::vector<std::size_t> out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

}  // namespace

std::size_t LogStore::add(Record r) {
    records_.push_back(std::move(r));
    const std::size_t id = records_.size() - 1;
    if (time_indexed_) index_time(id);
    for (auto& [field, index] : field_indexes_) index_field(field, id);
    return id;
}

void LogStore::index_time(std::size_t id) {
    const auto t = indexed_time(records_[id]);
    if (!t) return;
    const std::pair<std::int64_t, std::size_t> entry{t->ns, id};
    if (time_index_.empty() || time_index_.back() <= entry) {
        time_index_.push_back(entry);
    } else {
        time_index_.insert(std::upper_bound(time_index_.begin(), time_index_.end(), entry), entry);
    }
}

void LogStore::build_time_index() {
    time_index_.clear();
    for (std::size_t id = 0; id < records_.size(); ++id) {
        if (auto t = indexed_time(records_[id])) time_index_.emplace_back(t->ns, id);
    }
    std::sort(time_index_.begin(), time_index_.end());
    time_indexed_ = true;
}

void LogStore::index_field(const std::string& field, std::size_t id) {
    const Value* v = records_[id].find(field);
    if (!v || v->is_null()) return;
    field_indexes_[field][index_key(*v)].push_back(id);
}

void LogStore::build_field_index(const std::string& field) {
    auto& index = field_indexes_[field];
    index.clear();
    for (std::size_t id = 0; id < records_.size(); ++id) index_field(field, id);
}

bool LogStore::has_field_index(std::string_view field) const {
    return field_indexes_.find(std::string(field)) != field_indexes_.end();
}

std::vector<std::string> LogStore::indexed_fields() const {
    std::vector<std::string> out;
    for (const auto& entry : field_indexes_) out.push_back(entry.first);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::size_t> LogStore::time_range(std::optional<TimePoint> from, bool from_inclusive,
                                              std::optional<TimePoint> to, bool to_inclusive) const {
    auto lo = time_index_.begin();
    auto hi = time_index_.end();
    if (from) {
        const std::int64_t ns = from->ns;
        lo = std::partition_point(time_index_.begin(), time_index_.end(), [&](const auto& e) {
            return from_inclusive ? e.first < ns : e.first <= ns;
        });
    }
    if (to) {
        const std::int64_t ns = to->ns;
        hi = std::partition_point(lo, time_index_.end(), [&](const auto& e) {
            return to_inclusive ? e.first <= ns : e.first < ns;
        });
    }
    std::vector<std::size_t> ids;
    if (lo < hi) {
        ids.reserve(static_cast<std::size_t>(hi - lo));
        for (auto it = lo; it != hi; ++it) ids.push_back(it->second);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<std::size_t> LogStore::lookup(std::string_view field, const Value& v) const {
    if (v.is_null()) return {};
    const auto f = field_indexes_.find(std::string(field));
    if (f == field_indexes_.end()) return {};
    const auto it = f->second.find(index_key(v));
    if (it == f->second.end()) return {};
    return it->second;
}

std::optional<std::pair<TimePoint, TimePoint>> LogStore::time_span() const {
    if (time_indexed_) {
        if (time_index_.empty()) return std::nullopt;
        return std::make_pair(TimePoint{time_index_.front().first}, TimePoint{time_index_.back().first});
    }
    std::optional<std::pair<TimePoint, TimePoint>> span;
    for (const Record& r : records_) {
        const auto t = indexed_time(r);
        if (!t) continue;
        if (!span) {
            span = std::make_pair(*t, *t);
        } else {
            span->first = std::min(span->first, *t);
            span->second = std::max(span->second, *t);
        }
    }
    return span;
}

// ---- Planning ------------------------------------------------------------------------

namespace {

void collect_conjuncts(const Expr& e, std::vector<const Expr*>& out) {
    if (e.kind == Expr::Kind::Binary && e.binary_op == BinaryOp::And) {
        collect_conjuncts(*e.children[0], out);
        collect_conjuncts(*e.children[1], out);
        return;
    }
    out.push_back(&e);
}

BinaryOp flip(BinaryOp op) {
    switch (op) {
        case BinaryOp::Lt: return BinaryOp::Gt;
        case BinaryOp::Le: return BinaryOp::Ge;
        case BinaryOp::Gt: return BinaryOp::Lt;
        case BinaryOp::Ge: return BinaryOp::Le;
        default: return op;
    }
}

void tighten_from(std::optional<ScanPlan::Bound>& cur, ScanPlan::Bound b) {
    if (!cur || b.time > cur->time) {
        cur = b;
    } else if (b.time == cur->time) {
        cur->inclusive = cur->inclusive && b.inclusive;
    }
}

void tighten_to(std::optional<ScanPlan::Bound>& cur, ScanPlan::Bound b) {
    if (!cur || b.time < cur->time) {
        cur = b;
    } else if (b.time == cur->time) {
        cur->inclusive = cur->inclusive && b.inclusive;
    }
}

}  // namespace

ScanPlan plan_scan(const Query& query, const LogStore& store, const EvalContext& ctx) {
    ScanPlan plan;
    if (query.stages.empty() || query.stages.front().kind != Stage::Kind::Where) return plan;
    std::vector<const Expr*> terms;
    collect_conjuncts(*query.stages.front().predicate, terms);
    const Record empty;

    for (const Expr* t : terms) {
        if (t->kind == Expr::Kind::Binary) {
            BinaryOp op = t->binary_op;
            if (op != BinaryOp::Eq && op != BinaryOp::Lt && op != BinaryOp::Le && op != BinaryOp::Gt &&
                op != BinaryOp::Ge) {
                continue;
            }
            const Expr* field = t->children[0].get();
            const Expr* constant = t->children[1].get();
            if (field->kind != Expr::Kind::Field) {
                std::swap(field, constant);
                op = flip(op);
            }
            if (field->kind != Expr::Kind::Field || !is_constant(*constant)) continue;
            const Value v = evaluate(*constant, empty, ctx);
            if (field->name == fields::kTime && store.has_time_index()) {
                const auto time = v.to_time();
                if (!time) continue;
                switch (op) {
                    case BinaryOp::Gt: tighten_from(plan.from, {*time, false}); break;
                    case BinaryOp::Ge: tighten_from(plan.from, {*time, true}); break;
                    case BinaryOp::Lt: tighten_to(plan.to, {*time, false}); break;
                    case BinaryOp::Le: tighten_to(plan.to, {*time, true}); break;
                    case BinaryOp::Eq:
                        tighten_from(plan.from, {*time, true});
                        tighten_to(plan.to, {*time, true});
                        break;
                    default: break;
                }
            } else if (op == BinaryOp::Eq && !v.is_null() && store.has_field_index(field->name)) {
                plan.equalities.emplace_back(field->name, std::vector<Value>{v});
            }
        } else if (t->kind == Expr::Kind::In && !t->negated && t->children[0]->kind == Expr::Kind::Field &&
                   store.has_field_index(t->children[0]->name)) {
            std::vector<Value> values;
            bool usable = true;
            for (std::size_t i = 1; i < t->children.size() && usable; ++i) {
                usable = is_constant(*t->children[i]);
                if (usable) {
                    Value v = evaluate(*t->children[i], empty, ctx);
                    // A null item matches nothing indexed but must not be
                    // dropped silently: null == null is true for missing fields.
                    usable = !v.is_null();
                    values.push_back(std::move(v));
                }
            }
            if (usable) plan.equalities.emplace_back(t->children[0]->name, std::move(values));
        }
    }
    return plan;
}

std::string ScanPlan::describe() const {
    if (full_scan()) return "full scan";
    std::string out;
    auto add = [&](const std::string& s) {
        if (!out.empty()) out += " and ";
        out += s;
    };
    if (from) add(std::string("_time ") + (from->inclusive ? ">= " : "> ") + format_time(from->time));
    if (to) add(std::string("_time ") + (to->inclusive ? "<= " : "< ") + format_time(to->time));
    for (const auto& [field, values] : equalities) {
        std::string s = field + " in (";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) s += ", ";
            s += values[i].to_string();
        }
        add(s + ") [index]");
    }
    return "index scan: " + out;
}

std::vector<std::size_t> candidates(const LogStore& store, const ScanPlan& plan) {
    std::optional<std::vector<std::size_t>> ids;
    if (plan.from || plan.to) {
        ids = store.time_range(plan.from ? std::optional<TimePoint>(plan.from->time) : std::nullopt,
                               plan.from && plan.from->inclusive,
                               plan.to ? std::optional<TimePoint>(plan.to->time) : std::nullopt,
                               plan.to && plan.to->inclusive);
    }
    for (const auto& [field, values] : plan.equalities) {
        std::vector<std::size_t> matching;
        for (const Value& v : values) {
            const auto hits = store.lookup(field, v);
            matching.insert(matching.end(), hits.begin(), hits.end());
        }
        std::sort(matching.begin(), matching.end());
        matching.erase(std::unique(matching.begin(), matching.end()), matching.end());
        ids = ids ? intersect(*ids, matching) : std::move(matching);
    }
    if (!ids) {
        ids.emplace(store.size());
        for (std::size_t i = 0; i < store.size(); ++i) (*ids)[i] = i;
    }
    return *ids;
}

ExecStats execute(const Query& query, const LogStore& store, const std::function<bool(Record&&)>& sink,
                  const EvalContext& ctx, std::optional<ScanPlan::Bound> extra_from,
                  std::optional<ScanPlan::Bound> extra_to) {
    ExecStats stats;
    stats.plan = plan_scan(query, store, ctx);
    ScanPlan scan = stats.plan;
    if (store.has_time_index()) {
        if (extra_from) tighten_from(scan.from, *extra_from);
        if (extra_to) tighten_to(scan.to, *extra_to);
    }
    const std::vector<std::size_t> ids = candidates(store, scan);
    stats.candidates = ids.size();

    auto in_window = [&](const Record& r) {
        if (!extra_from && !extra_to) return true;
        const auto t = indexed_time(r);
        if (!t) return false;
        if (extra_from && (extra_from->inclusive ? *t < extra_from->time : *t <= extra_from->time)) return false;
        if (extra_to && (extra_to->inclusive ? *t > extra_to->time : *t >= extra_to->time)) return false;
        return true;
    };

    Pipeline pipeline(query, [&](Record&& r) {
        ++stats.emitted;
        return sink(std::move(r));
    }, ctx);
    for (std::size_t id : ids) {
        const Record& r = store[id];
        if (!in_window(r)) continue;
        ++stats.examined;
        if (!pipeline.push(r)) break;
    }
    pipeline.finish();
    return stats;
}

}  // namespace logsift
