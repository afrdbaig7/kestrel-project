#pragma once
// ============================================================================
// kestrel/core/byte_codec.hpp
//
// Span-safe primitive encoding/decoding. All loads/stores are byte-wise with
// explicit endianness — no unaligned loads, no packed-struct aliasing, no UB.
//
// Decode returns a status; callers MUST check it. Every read validates bounds.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace kestrel::core {

enum class CodecStatus : std::uint8_t { Ok, Overflow, Underflow };

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> s) noexcept
        : data_(s.data()), size_(s.size()) {}

    std::size_t remaining() const noexcept { return size_ - pos_; }
    bool can_read(std::size_t n) const noexcept { return remaining() >= n; }
    std::size_t pos() const noexcept { return pos_; }

    CodecStatus u8(std::uint8_t& v) noexcept {
        if (!can_read(1)) return CodecStatus::Underflow;
        v = data_[pos_++];
        return CodecStatus::Ok;
    }
    CodecStatus u16be(std::uint16_t& v) noexcept {
        if (!can_read(2)) return CodecStatus::Underflow;
        v = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data_[pos_]) << 8) | data_[pos_ + 1]);
        pos_ += 2;
        return CodecStatus::Ok;
    }
    CodecStatus u16le(std::uint16_t& v) noexcept {
        if (!can_read(2)) return CodecStatus::Underflow;
        v = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8) | data_[pos_]);
        pos_ += 2;
        return CodecStatus::Ok;
    }
    CodecStatus u32be(std::uint32_t& v) noexcept {
        if (!can_read(4)) return CodecStatus::Underflow;
        v = (static_cast<std::uint32_t>(data_[pos_]) << 24) |
            (static_cast<std::uint32_t>(data_[pos_ + 1]) << 16) |
            (static_cast<std::uint32_t>(data_[pos_ + 2]) << 8) |
            static_cast<std::uint32_t>(data_[pos_ + 3]);
        pos_ += 4;
        return CodecStatus::Ok;
    }
    CodecStatus u32le(std::uint32_t& v) noexcept {
        if (!can_read(4)) return CodecStatus::Underflow;
        v = (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24) |
            (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16) |
            (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8) |
            static_cast<std::uint32_t>(data_[pos_]);
        pos_ += 4;
        return CodecStatus::Ok;
    }
    CodecStatus u64be(std::uint64_t& v) noexcept {
        if (!can_read(8)) return CodecStatus::Underflow;
        std::uint64_t x = 0;
        for (int i = 0; i < 8; ++i) x = (x << 8) | data_[pos_ + i];
        pos_ += 8;
        v = x;
        return CodecStatus::Ok;
    }
    CodecStatus u64le(std::uint64_t& v) noexcept {
        if (!can_read(8)) return CodecStatus::Underflow;
        std::uint64_t x = 0;
        for (int i = 7; i >= 0; --i) x = (x << 8) | data_[pos_ + i];
        pos_ += 8;
        v = x;
        return CodecStatus::Ok;
    }
    /// Read a fixed-length ASCII field (e.g. symbol). Returns Underflow if the
    /// buffer is short; trailing NUL/space padding is stripped.
    CodecStatus ascii(std::size_t n, std::string_view& v) noexcept {
        if (!can_read(n)) return CodecStatus::Underflow;
        const char* p = reinterpret_cast<const char*>(data_ + pos_);
        std::size_t len = n;
        while (len > 0 && (p[len - 1] == '\0' || p[len - 1] == ' ')) --len;
        v = std::string_view{p, len};
        pos_ += n;
        return CodecStatus::Ok;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

class ByteWriter {
public:
    explicit ByteWriter(std::span<std::uint8_t> s) noexcept
        : data_(s.data()), size_(s.size()) {}

    std::size_t written() const noexcept { return pos_; }
    std::size_t remaining() const noexcept { return size_ - pos_; }
    bool can_write(std::size_t n) const noexcept { return remaining() >= n; }

    CodecStatus u8(std::uint8_t v) noexcept {
        if (!can_write(1)) return CodecStatus::Overflow;
        data_[pos_++] = v;
        return CodecStatus::Ok;
    }
    CodecStatus u16be(std::uint16_t v) noexcept {
        if (!can_write(2)) return CodecStatus::Overflow;
        data_[pos_] = static_cast<std::uint8_t>(v >> 8);
        data_[pos_ + 1] = static_cast<std::uint8_t>(v);
        pos_ += 2;
        return CodecStatus::Ok;
    }
    CodecStatus u16le(std::uint16_t v) noexcept {
        if (!can_write(2)) return CodecStatus::Overflow;
        data_[pos_] = static_cast<std::uint8_t>(v);
        data_[pos_ + 1] = static_cast<std::uint8_t>(v >> 8);
        pos_ += 2;
        return CodecStatus::Ok;
    }
    CodecStatus u32be(std::uint32_t v) noexcept {
        if (!can_write(4)) return CodecStatus::Overflow;
        data_[pos_]     = static_cast<std::uint8_t>(v >> 24);
        data_[pos_ + 1] = static_cast<std::uint8_t>(v >> 16);
        data_[pos_ + 2] = static_cast<std::uint8_t>(v >> 8);
        data_[pos_ + 3] = static_cast<std::uint8_t>(v);
        pos_ += 4;
        return CodecStatus::Ok;
    }
    CodecStatus u32le(std::uint32_t v) noexcept {
        if (!can_write(4)) return CodecStatus::Overflow;
        for (int i = 0; i < 4; ++i) {
            data_[pos_ + i] = static_cast<std::uint8_t>(v);
            v >>= 8;
        }
        pos_ += 4;
        return CodecStatus::Ok;
    }
    CodecStatus u64be(std::uint64_t v) noexcept {
        if (!can_write(8)) return CodecStatus::Overflow;
        for (int i = 7; i >= 0; --i) {
            data_[pos_ + i] = static_cast<std::uint8_t>(v);
            v >>= 8;
        }
        pos_ += 8;
        return CodecStatus::Ok;
    }
    CodecStatus u64le(std::uint64_t v) noexcept {
        if (!can_write(8)) return CodecStatus::Overflow;
        for (int i = 0; i < 8; ++i) {
            data_[pos_ + i] = static_cast<std::uint8_t>(v);
            v >>= 8;
        }
        pos_ += 8;
        return CodecStatus::Ok;
    }

private:
    std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

}  // namespace kestrel::core
