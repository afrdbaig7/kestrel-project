#pragma once
// ============================================================================
// kestrel/core/histogram.hpp
//
// Latency histogram implementing the published HdrHistogram algorithm
// (log-linear buckets, configurable significant figures). Cross-checked
// against HdrHistogram_c (github.com/HdrHistogram/HdrHistogram_c) sources;
// see ADR-0003.
//
// Faithful details (matching hdr_histogram.c):
//   largest_value_with_single_unit_resolution = 2 * 10^sig_figs
//   sub_bucket_count_magnitude  = ceil(log2(largest_single_unit))
//   sub_bucket_half_count_magnitude = max(that, 1) - 1
//   sub_bucket_count = 2^(half_mag + 1);  half = sub_bucket_count / 2
//   sub_bucket_mask  = (sub_bucket_count - 1) << unit_magnitude   <-- FULL-1
//   bucket_count = doublings from (sub_bucket_count << unit) past highest
//   counts_len   = (bucket_count + 1) * half
//   bucket_index = pow2ceil(v | sub_bucket_mask) - unit - (half_mag + 1)
//   sub_index    = v >> (bucket_index + unit)
//   counts_index = ((bucket + 1) << half_mag) + (sub - half)
//   value_at_index: bucket=(i>>half_mag)-1, sub=(i&(half-1))+half;
//                   if bucket<0 { sub-=half; bucket=0 }
//   percentile   = highest_equivalent_value(value_at_index(first i with
//                  running >= ceil(p/100*total + 0.5)))
//
// Semantics:
//   * record(v): v<0 clamps to 0 (capped-record style); v>highest clamps into
//     the top bucket and increments overflow_count(); 0 does not update min.
//   * value_at_percentile returns the bucket TOP (HdrHistogram convention).
//   * Not synchronized (single writer); merge/exports are off-path.
// ============================================================================

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <memory>
#include <string>
#include <sstream>

namespace kestrel::core {

class Histogram {
public:
    Histogram(std::int64_t highest_trackable, int significant_figures = 3,
              std::int64_t lowest_discernible = 1)
        : highest_trackable_(highest_trackable),
          sig_figs_(std::clamp(significant_figures, 1, 5)) {
        const std::int64_t largest_single_unit = 2 * ipow10(sig_figs_);
        const int sub_bucket_count_magnitude =
            static_cast<int>(std::ceil(std::log2(static_cast<double>(largest_single_unit))));
        sub_bucket_half_count_magnitude_ = std::max(sub_bucket_count_magnitude, 1) - 1;
        sub_bucket_count_ = 1 << (sub_bucket_half_count_magnitude_ + 1);
        sub_bucket_half_count_ = sub_bucket_count_ >> 1;
        unit_magnitude_ = lowest_discernible <= 0
                              ? 0
                              : static_cast<int>(63 - std::countl_zero(
                                      static_cast<std::uint64_t>(lowest_discernible)));
        sub_bucket_mask_ = (static_cast<std::int64_t>(sub_bucket_count_) - 1) << unit_magnitude_;

        // buckets_needed_to_cover_value
        std::int64_t smallest_untrackable =
            static_cast<std::int64_t>(sub_bucket_count_) << unit_magnitude_;
        int buckets = 1;
        while (smallest_untrackable <= highest_trackable_ && smallest_untrackable > 0) {
            if (smallest_untrackable > (INT64_MAX >> 1)) { smallest_untrackable = INT64_MAX; break; }
            smallest_untrackable <<= 1;
            ++buckets;
        }
        bucket_count_ = buckets;
        counts_len_ = static_cast<std::size_t>(bucket_count_ + 1) *
                      static_cast<std::size_t>(sub_bucket_half_count_);
        counts_ = std::make_unique<std::int64_t[]>(counts_len_);
    }

    // ---- recording (hot path; single writer) -------------------------------
    void record(std::int64_t value) noexcept {
        if (value < 0) value = 0;
        ++total_count_;
        if (value != 0 && value < min_value_) min_value_ = value;
        if (value > max_value_) max_value_ = value;
        if (value > highest_trackable_) {
            ++overflow_count_;
            value = highest_trackable_;
        }
        ++counts_[static_cast<std::size_t>(counts_index_for(value))];
    }

    void record_n(std::int64_t value, std::int64_t n) noexcept {
        for (std::int64_t i = 0; i < n; ++i) record(value);
    }

    // ---- inspection --------------------------------------------------------
    std::int64_t total_count() const noexcept { return total_count_; }
    std::int64_t max_recorded() const noexcept { return total_count_ ? max_value_ : 0; }
    /// Smallest non-zero recorded value (HdrHistogram convention); 0 when empty.
    std::int64_t min_recorded() const noexcept {
        return min_value_ == INT64_MAX ? 0 : min_value_;
    }
    std::int64_t overflow_count() const noexcept { return overflow_count_; }
    std::int64_t highest_trackable() const noexcept { return highest_trackable_; }
    int significant_figures() const noexcept { return sig_figs_; }
    std::size_t counts_length() const noexcept { return counts_len_; }
    std::size_t memory_bytes() const noexcept { return counts_len_ * sizeof(std::int64_t); }

    double mean() const noexcept {
        if (!total_count_) return 0.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < counts_len_; ++i) {
            if (counts_[i]) sum += static_cast<double>(counts_[i]) *
                                   static_cast<double>(value_at_index(i));
        }
        return sum / static_cast<double>(total_count_);
    }

    std::int64_t value_at_percentile(double percentile) const noexcept {
        if (!total_count_) return 0;
        percentile = std::clamp(percentile, 0.0, 100.0);
        // count_at_percentile: (p/100 * total) + 0.5, clamped to [1, total]
        const double cc = (percentile / 100.0) * static_cast<double>(total_count_) + 0.5;
        std::int64_t target;
        if (!(cc >= 1.0)) target = 1;
        else if (cc >= static_cast<double>(total_count_)) target = total_count_;
        else target = static_cast<std::int64_t>(cc);

        std::int64_t running = 0;
        for (std::size_t i = 0; i < counts_len_; ++i) {
            running += counts_[i];
            if (running >= target) {
                return highest_equivalent_value(value_at_index(i));
            }
        }
        return highest_equivalent_value(value_at_index(counts_len_ - 1));
    }

    void reset() noexcept {
        std::fill_n(counts_.get(), counts_len_, 0);
        total_count_ = 0;
        max_value_ = INT64_MIN;
        min_value_ = INT64_MAX;
        overflow_count_ = 0;
    }

    void merge(const Histogram& other) noexcept {
        for (std::size_t i = 0; i < counts_len_ && i < other.counts_len_; ++i) {
            counts_[i] += other.counts_[i];
        }
        total_count_ += other.total_count_;
        overflow_count_ += other.overflow_count_;
        if (other.total_count_) {
            max_value_ = std::max(max_value_, other.max_value_);
            min_value_ = std::min(min_value_, other.min_value_);
        }
    }

    // ---- export ------------------------------------------------------------
    std::string summary() const {
        std::ostringstream os;
        os << "count=" << total_count_ << " min=" << min_recorded()
           << " p50=" << value_at_percentile(50.0)
           << " p90=" << value_at_percentile(90.0)
           << " p99=" << value_at_percentile(99.0)
           << " p99.9=" << value_at_percentile(99.9)
           << " p99.99=" << value_at_percentile(99.99)
           << " max=" << max_recorded()
           << " mean=" << static_cast<std::int64_t>(mean())
           << " overflow=" << overflow_count_;
        return os.str();
    }

    std::string to_csv() const {
        std::ostringstream os;
        os << "percentile,value_ns\n";
        static const double ps[] = {0.0, 50.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0};
        for (const double p : ps)
            os << p << ',' << value_at_percentile(p) << '\n';
        return os.str();
    }

    const std::int64_t* counts_data() const noexcept { return counts_.get(); }

private:
    static std::int64_t ipow10(int n) noexcept {
        std::int64_t v = 1;
        for (int i = 0; i < n; ++i) v *= 10;
        return v;
    }

    int get_bucket_index(std::int64_t value) const noexcept {
        const int pow2ceil = 64 - std::countl_zero(static_cast<std::uint64_t>(
                                 value | sub_bucket_mask_));
        return pow2ceil - unit_magnitude_ - (sub_bucket_half_count_magnitude_ + 1);
    }
    static int get_sub_bucket_index(std::int64_t value, int bucket_index,
                                    int unit_magnitude) noexcept {
        return static_cast<int>(value >> (bucket_index + unit_magnitude));
    }
    std::size_t counts_index(int bucket_index, int sub_bucket_index) const noexcept {
        const std::size_t bucket_base = static_cast<std::size_t>(bucket_index + 1)
                                        << sub_bucket_half_count_magnitude_;
        return bucket_base + static_cast<std::size_t>(
                   sub_bucket_index - sub_bucket_half_count_);
    }
    std::size_t counts_index_for(std::int64_t value) const noexcept {
        const int b = get_bucket_index(value);
        const int s = get_sub_bucket_index(value, b, unit_magnitude_);
        return counts_index(b, s);
    }

    std::int64_t value_from_index(int bucket_index, int sub_bucket_index) const noexcept {
        return static_cast<std::int64_t>(sub_bucket_index)
               << (bucket_index + unit_magnitude_);
    }

    /// Inverse mapping: counts array index -> bucket LOW value.
    std::int64_t value_at_index(std::size_t i) const noexcept {
        int bucket = static_cast<int>(i >> sub_bucket_half_count_magnitude_) - 1;
        int sub = static_cast<int>(i & (static_cast<std::size_t>(sub_bucket_half_count_) - 1)) +
                  sub_bucket_half_count_;
        if (bucket < 0) {
            sub -= sub_bucket_half_count_;
            bucket = 0;
        }
        return value_from_index(bucket, sub);
    }

    /// Top value of the bucket containing `low` (highest_equivalent_value).
    std::int64_t highest_equivalent_value(std::int64_t low) const noexcept {
        const int b = get_bucket_index(low);
        const int s = get_sub_bucket_index(low, b, unit_magnitude_);
        const int adjusted = (s >= sub_bucket_count_) ? (b + 1) : b;
        const std::int64_t size = static_cast<std::int64_t>(1) << (adjusted + unit_magnitude_);
        if (low > INT64_MAX - size) return INT64_MAX;
        return low + size - 1;
    }

    std::int64_t highest_trackable_;
    int sig_figs_;
    int unit_magnitude_{};
    int sub_bucket_half_count_magnitude_{};
    int sub_bucket_half_count_{};
    int sub_bucket_count_{};
    int bucket_count_{};
    std::int64_t sub_bucket_mask_{};
    std::size_t counts_len_{};
    std::unique_ptr<std::int64_t[]> counts_;

    std::int64_t total_count_ = 0;
    std::int64_t max_value_ = INT64_MIN;
    std::int64_t min_value_ = INT64_MAX;
    std::int64_t overflow_count_ = 0;
};

}  // namespace kestrel::core
