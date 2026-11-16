// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "logsift/pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include "logsift/text.hpp"

namespace logsift {

// ---- Aggregators -----------------------------------------------------------------

namespace {

struct TotalLess {
    bool operator()(const Value& a, const Value& b) const { return compare_total(a, b) < 0; }
};

std::optional<double> numeric(const Value& v) {
    if (v.is_null() || v.is_bool() || v.is_time()) return std::nullopt;
    return v.to_number();
}

class CountAgg final : public Aggregator {
public:
    void add(const Value& v) override {
        if (!v.is_null()) ++n_;
    }
    Value result() const override { return Value::integer(n_); }

private:
    std::int64_t n_ = 0;
};

class SumAgg final : public Aggregator {
public:
    void add(const Value& v) override {
        if (v.is_int() && integral_) {
            std::int64_t out = 0;
#if defined(__GNUC__) || defined(__clang__)
            const bool overflow = __builtin_add_overflow(int_sum_, v.as_int(), &out);
#else
            const std::int64_t b = v.as_int();
            const bool overflow = (b > 0 && int_sum_ > std::numeric_limits<std::int64_t>::max() - b) ||
                                  (b < 0 && int_sum_ < std::numeric_limits<std::int64_t>::min() - b);
            if (!overflow) out = int_sum_ + b;
#endif
            if (!overflow) {
                int_sum_ = out;
                return;
            }
        }
        const auto x = numeric(v);
        if (!x) return;
        if (integral_) {
            integral_ = false;
            sum_ = static_cast<double>(int_sum_);
        }
        sum_ += *x;
    }
    Value result() const override {
        if (integral_) return Value::integer(int_sum_);
        return std::isfinite(sum_) ? Value::real(sum_) : Value{};
    }

private:
    bool integral_ = true;
    std::int64_t int_sum_ = 0;
    double sum_ = 0;
};

class AvgAgg final : public Aggregator {
public:
    void add(const Value& v) override {
        if (auto x = numeric(v)) {
            sum_ += *x;
            ++n_;
        }
    }
    Value result() const override { return n_ ? Value::real(sum_ / static_cast<double>(n_)) : Value{}; }

private:
    double sum_ = 0;
    std::int64_t n_ = 0;
};

class ExtremeAgg final : public Aggregator {
public:
    explicit ExtremeAgg(bool want_max) : want_max_(want_max) {}
    void add(const Value& v) override {
        if (v.is_null()) return;
        if (!have_ || (want_max_ ? compare_total(v, best_) > 0 : compare_total(v, best_) < 0)) {
            best_ = v;
            have_ = true;
        }
    }
    Value result() const override { return have_ ? best_ : Value{}; }

private:
    bool want_max_;
    bool have_ = false;
    Value best_;
};

class DistinctCountAgg final : public Aggregator {
public:
    void add(const Value& v) override {
        if (!v.is_null()) keys_.insert(index_key(v));
    }
    Value result() const override { return Value::integer(static_cast<std::int64_t>(keys_.size())); }

private:
    std::unordered_set<std::string> keys_;
};

class ValuesAgg final : public Aggregator {
public:
    static constexpr std::size_t kLimit = 100;
    void add(const Value& v) override {
        if (!v.is_null() && (values_.size() < kLimit || values_.count(v))) values_.insert(v);
    }
    Value result() const override {
        std::string out;
        for (const Value& v : values_) {
            if (!out.empty()) out += ", ";
            out += v.to_string();
        }
        return Value::string(std::move(out));
    }

private:
    std::set<Value, TotalLess> values_;
};

class FirstLastAgg final : public Aggregator {
public:
    explicit FirstLastAgg(bool last) : last_(last) {}
    void add(const Value& v) override {
        if (v.is_null() || (have_ && !last_)) return;
        value_ = v;
        have_ = true;
    }
    Value result() const override { return value_; }

private:
    bool last_;
    bool have_ = false;
    Value value_;
};

class PercentileAgg final : public Aggregator {
public:
    explicit PercentileAgg(double rank) : rank_(rank) {}
    void add(const Value& v) override {
        if (auto x = numeric(v)) values_.push_back(*x);
    }
    Value result() const override {
        if (values_.empty()) return {};
        std::vector<double> sorted = values_;
        std::sort(sorted.begin(), sorted.end());
        // Linear interpolation between closest ranks.
        const double pos = rank_ / 100.0 * static_cast<double>(sorted.size() - 1);
        const auto lo = static_cast<std::size_t>(std::floor(pos));
        const auto hi = static_cast<std::size_t>(std::ceil(pos));
        const double frac = pos - static_cast<double>(lo);
        return Value::real(sorted[lo] + (sorted[hi] - sorted[lo]) * frac);
    }

private:
    double rank_;
    std::vector<double> values_;
};

class StdevAgg final : public Aggregator {
public:
    void add(const Value& v) override {
        // Welford's online algorithm.
        if (auto x = numeric(v)) {
            ++n_;
            const double delta = *x - mean_;
            mean_ += delta / static_cast<double>(n_);
            m2_ += delta * (*x - mean_);
        }
    }
    Value result() const override {
        if (n_ < 2) return {};
        return Value::real(std::sqrt(m2_ / static_cast<double>(n_ - 1)));
    }

private:
    std::int64_t n_ = 0;
    double mean_ = 0;
    double m2_ = 0;
};

class RangeAgg final : public Aggregator {
public:
    void add(const Value& v) override {
        if (auto x = numeric(v)) {
            lo_ = have_ ? std::min(lo_, *x) : *x;
            hi_ = have_ ? std::max(hi_, *x) : *x;
            have_ = true;
        }
    }
    Value result() const override { return have_ ? Value::real(hi_ - lo_) : Value{}; }

private:
    bool have_ = false;
    double lo_ = 0;
    double hi_ = 0;
};

}  // namespace

std::unique_ptr<Aggregator> make_aggregator(const std::string& function, double parameter) {
    if (function == "count") return std::make_unique<CountAgg>();
    if (function == "sum") return std::make_unique<SumAgg>();
    if (function == "avg") return std::make_unique<AvgAgg>();
    if (function == "min") return std::make_unique<ExtremeAgg>(false);
    if (function == "max") return std::make_unique<ExtremeAgg>(true);
    if (function == "dc") return std::make_unique<DistinctCountAgg>();
    if (function == "values") return std::make_unique<ValuesAgg>();
    if (function == "first") return std::make_unique<FirstLastAgg>(false);
    if (function == "last") return std::make_unique<FirstLastAgg>(true);
    if (function == "median" || function == "p50") return std::make_unique<PercentileAgg>(50);
    if (function == "p90") return std::make_unique<PercentileAgg>(90);
    if (function == "p95") return std::make_unique<PercentileAgg>(95);
    if (function == "p99") return std::make_unique<PercentileAgg>(99);
    if (function == "percentile") return std::make_unique<PercentileAgg>(parameter);
    if (function == "stdev") return std::make_unique<StdevAgg>();
    if (function == "range") return std::make_unique<RangeAgg>();
    throw std::invalid_argument("unknown aggregate function: " + function);
}

// ---- Stage nodes -------------------------------------------------------------------

namespace detail {

class StageNode {
public:
    virtual ~StageNode() = default;
    void set_next(StageNode* next) { next_ = next; }

    /// Returns false when this node wants no more input.
    virtual bool push(Record r) = 0;
    virtual void finish() {
        if (next_) next_->finish();
    }

protected:
    bool emit(Record r) {
        if (stopped_) return false;
        if (!next_->push(std::move(r))) stopped_ = true;
        return !stopped_;
    }
    bool stopped() const { return stopped_; }

    StageNode* next_ = nullptr;

private:
    bool stopped_ = false;
};

}  // namespace detail

namespace {

using detail::StageNode;

class SinkNode final : public StageNode {
public:
    explicit SinkNode(Sink sink) : sink_(std::move(sink)) {}
    bool push(Record r) override { return sink_(std::move(r)); }
    void finish() override {}

private:
    Sink sink_;
};

class WhereNode final : public StageNode {
public:
    WhereNode(const Stage& s, const EvalContext& ctx) : stage_(s), ctx_(ctx) {}
    bool push(Record r) override {
        if (stopped()) return false;
        if (!matches(*stage_.predicate, r, ctx_)) return true;
        return emit(std::move(r));
    }

private:
    const Stage& stage_;
    const EvalContext& ctx_;
};

class EvalNode final : public StageNode {
public:
    EvalNode(const Stage& s, const EvalContext& ctx) : stage_(s), ctx_(ctx) {}
    bool push(Record r) override {
        for (const auto& [name, expr] : stage_.assignments) r.set(name, evaluate(*expr, r, ctx_));
        return emit(std::move(r));
    }

private:
    const Stage& stage_;
    const EvalContext& ctx_;
};

bool pattern_matches(const std::string& pattern, const std::string& name) {
    if (pattern == "*") return true;
    if (!pattern.empty() && pattern.back() == '*') return starts_with(name, std::string_view(pattern).substr(0, pattern.size() - 1));
    return pattern == name;
}

class FieldsNode final : public StageNode {
public:
    explicit FieldsNode(const Stage& s) : stage_(s) {}
    bool push(Record r) override {
        if (stage_.exclude) {
            Record out;
            for (const auto& f : r.fields()) {
                const bool drop = std::any_of(stage_.names.begin(), stage_.names.end(),
                                              [&](const std::string& p) { return pattern_matches(p, f.name); });
                if (!drop) out.set(f.name, f.value);
            }
            return emit(std::move(out));
        }
        Record out;
        for (const std::string& p : stage_.names) {
            for (const auto& f : r.fields()) {
                if (pattern_matches(p, f.name) && !out.has(f.name)) out.set(f.name, f.value);
            }
        }
        return emit(std::move(out));
    }

private:
    const Stage& stage_;
};

class RenameNode final : public StageNode {
public:
    explicit RenameNode(const Stage& s) : stage_(s) {}
    bool push(Record r) override {
        for (const auto& [from, to] : stage_.renames) r.rename(from, to);
        return emit(std::move(r));
    }

private:
    const Stage& stage_;
};

class HeadNode final : public StageNode {
public:
    explicit HeadNode(const Stage& s) : limit_(s.count) {}
    bool push(Record r) override {
        if (seen_ >= limit_) return false;
        ++seen_;
        const bool more = emit(std::move(r));
        return more && seen_ < limit_;
    }

private:
    std::int64_t limit_;
    std::int64_t seen_ = 0;
};

class TailNode final : public StageNode {
public:
    explicit TailNode(const Stage& s) : limit_(static_cast<std::size_t>(s.count)) {}
    bool push(Record r) override {
        if (limit_ == 0) return true;
        if (buffer_.size() == limit_) buffer_.pop_front();
        buffer_.push_back(std::move(r));
        return true;
    }
    void finish() override {
        for (auto& r : buffer_) {
            if (!emit(std::move(r))) break;
        }
        buffer_.clear();
        StageNode::finish();
    }

private:
    std::size_t limit_;
    std::deque<Record> buffer_;
};

class DedupNode final : public StageNode {
public:
    explicit DedupNode(const Stage& s) : stage_(s) {}
    bool push(Record r) override {
        std::string key;
        for (const std::string& name : stage_.names) {
            key += index_key(r.get(name));
            key += '\x1f';
        }
        if (!seen_.insert(std::move(key)).second) return true;
        return emit(std::move(r));
    }

private:
    const Stage& stage_;
    std::unordered_set<std::string> seen_;
};

class BinNode final : public StageNode {
public:
    explicit BinNode(const Stage& s) : stage_(s) {}
    bool push(Record r) override {
        const Value v = r.get(stage_.field);
        if (stage_.span_is_duration) {
            if (auto t = v.to_time()) r.set(stage_.alias, Value::time(floor_time(*t, stage_.duration_span)));
        } else if (auto x = numeric(v)) {
            const double b = std::floor(*x / stage_.numeric_span) * stage_.numeric_span;
            if (std::trunc(b) == b && std::fabs(b) < 9.2e18) {
                r.set(stage_.alias, Value::integer(static_cast<std::int64_t>(b)));
            } else {
                r.set(stage_.alias, Value::real(b));
            }
        }
        return emit(std::move(r));
    }

private:
    const Stage& stage_;
};

class SortNode final : public StageNode {
public:
    explicit SortNode(const Stage& s) : stage_(s) {}
    bool push(Record r) override {
        buffer_.push_back(std::move(r));
        return true;
    }
    void finish() override {
        std::stable_sort(buffer_.begin(), buffer_.end(), [&](const Record& a, const Record& b) {
            for (const SortKey& key : stage_.sort_keys) {
                const Value va = a.get(key.field);
                const Value vb = b.get(key.field);
                // Nulls sort last in either direction.
                if (va.is_null() != vb.is_null()) return vb.is_null();
                int c = compare_total(va, vb);
                if (key.descending) c = -c;
                if (c != 0) return c < 0;
            }
            return false;
        });
        for (auto& r : buffer_) {
            if (!emit(std::move(r))) break;
        }
        buffer_.clear();
        StageNode::finish();
    }

private:
    const Stage& stage_;
    std::vector<Record> buffer_;
};

struct KeyLess {
    bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
            const int c = compare_total(a[i], b[i]);
            if (c != 0) return c < 0;
        }
        return a.size() < b.size();
    }
};

class StatsNode final : public StageNode {
public:
    StatsNode(const Stage& s, const EvalContext& ctx) : stage_(s), ctx_(ctx) {}

    bool push(Record r) override {
        std::vector<Value> key;
        key.reserve(stage_.names.size());
        for (const std::string& name : stage_.names) key.push_back(r.get(name));
        auto it = groups_.find(key);
        if (it == groups_.end()) it = groups_.emplace(std::move(key), fresh()).first;
        for (std::size_t i = 0; i < stage_.aggregates.size(); ++i) {
            const Aggregate& a = stage_.aggregates[i];
            it->second[i]->add(a.argument ? evaluate(*a.argument, r, ctx_) : Value::boolean(true));
        }
        return true;
    }

    void finish() override {
        if (groups_.empty() && stage_.names.empty()) groups_.emplace(std::vector<Value>{}, fresh());
        for (auto& [key, aggs] : groups_) {
            Record out;
            // Records missing a group-by field form their own group; the
            // field is left out of that group's output rather than shown as null.
            for (std::size_t i = 0; i < stage_.names.size(); ++i) {
                if (!key[i].is_null()) out.set(stage_.names[i], key[i]);
            }
            for (std::size_t i = 0; i < stage_.aggregates.size(); ++i) {
                out.set(stage_.aggregates[i].alias, aggs[i]->result());
            }
            if (!emit(std::move(out))) break;
        }
        groups_.clear();
        StageNode::finish();
    }

private:
    std::vector<std::unique_ptr<Aggregator>> fresh() const {
        std::vector<std::unique_ptr<Aggregator>> aggs;
        for (const Aggregate& a : stage_.aggregates) aggs.push_back(make_aggregator(a.function, a.parameter));
        return aggs;
    }

    const Stage& stage_;
    const EvalContext& ctx_;
    std::map<std::vector<Value>, std::vector<std::unique_ptr<Aggregator>>, KeyLess> groups_;
};

std::unique_ptr<StageNode> make_node(const Stage& s, const EvalContext& ctx) {
    switch (s.kind) {
        case Stage::Kind::Where: return std::make_unique<WhereNode>(s, ctx);
        case Stage::Kind::Eval: return std::make_unique<EvalNode>(s, ctx);
        case Stage::Kind::Fields: return std::make_unique<FieldsNode>(s);
        case Stage::Kind::Rename: return std::make_unique<RenameNode>(s);
        case Stage::Kind::Sort: return std::make_unique<SortNode>(s);
        case Stage::Kind::Head: return std::make_unique<HeadNode>(s);
        case Stage::Kind::Tail: return std::make_unique<TailNode>(s);
        case Stage::Kind::Dedup: return std::make_unique<DedupNode>(s);
        case Stage::Kind::Stats: return std::make_unique<StatsNode>(s, ctx);
        case Stage::Kind::Bin: return std::make_unique<BinNode>(s);
    }
    throw std::logic_error("unknown stage kind");
}

}  // namespace

// ---- Pipeline -------------------------------------------------------------------------

Pipeline::Pipeline(const Query& query, Sink sink, EvalContext ctx) : ctx_(ctx) {
    // Nodes keep references to the stages and to ctx_, both of which
    // outlive them (the query is owned by the caller; ctx_ is declared
    // before nodes_).
    for (const Stage& s : query.stages) nodes_.push_back(make_node(s, ctx_));
    nodes_.push_back(std::make_unique<SinkNode>(std::move(sink)));
    for (std::size_t i = 0; i + 1 < nodes_.size(); ++i) nodes_[i]->set_next(nodes_[i + 1].get());
}

Pipeline::~Pipeline() = default;

bool Pipeline::push(Record r) {
    if (!open_ || finished_) return false;
    ++consumed_;
    open_ = nodes_.front()->push(std::move(r));
    return open_;
}

void Pipeline::finish() {
    if (finished_) return;
    finished_ = true;
    nodes_.front()->finish();
}

std::vector<Record> run_query(const Query& query, const std::vector<Record>& input, const EvalContext& ctx) {
    std::vector<Record> out;
    Pipeline p(query, [&](Record&& r) {
        out.push_back(std::move(r));
        return true;
    }, ctx);
    for (const Record& r : input) {
        if (!p.push(r)) break;
    }
    p.finish();
    return out;
}

}  // namespace logsift
