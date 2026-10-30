#pragma once
// ============================================================================
// kestrel/journal/journal.hpp
//
// Versioned binary event journal (spec §9, ADR-0006).
//
// File layout:
//   header  : magic "KJRNL" | u16 format_version | u32 record_size_hint
//   records : [u32 len][u16 type][u16 ver][u32 crc32(payload)][payload]
//             len = full record length incl. header (bounds + corruption
//             detection via crc; a bad len/crc stops the replay at the last
//             good record and marks a gap).
//
// Record types (u16):
//   MD=1 KITCH frame payload | EXEC=2 KOUCH response frame | TIMER=3 u64 ns
//   CONFIG=4 config blob | CONTROL=5 control command | SEED=6 u64 seed
//   TX=7 engine outbound frame (replay comparison) | SNAPSHOT=8 state blob
//   MARKER=9 named marker
//
// Two write policies (spec §9):
//   critical : MD, EXEC, TIMER, CONTROL, CONFIG, SEED, TX, SNAPSHOT
//              -> writer ring full => FAIL-CLOSED: caller callback engages
//                 the kill switch, a GAP marker is written as soon as space
//                 allows, and new exposure stops. Recording gaps are loud.
//   telemetry: best-effort records -> dropped with a counter (never blocks).
//
// The engine thread only ever try_pushes into the SPSC ring; a separate IO
// thread (or a test-side drain) writes the file. The engine NEVER waits.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "kestrel/core/spsc_ring.hpp"

namespace kestrel::journal {

inline constexpr char kMagic[5] = {'K', 'J', 'R', 'N', 'L'};
inline constexpr std::uint16_t kFormatVersion = 1;

enum class RecordType : std::uint16_t {
    Md = 1,
    Exec = 2,
    Timer = 3,
    Config = 4,
    Control = 5,
    Seed = 6,
    Tx = 7,
    Snapshot = 8,
    Marker = 9,
    Gap = 10,          // written after a critical-overflow recording gap
};

inline constexpr std::size_t kMaxRecordPayload = 8192;

// ---- CRC32 (IEEE 802.3, reflected; table-based, deterministic) -------------
inline constexpr std::uint32_t crc32_update(std::uint32_t crc, std::uint8_t b) noexcept {
    crc ^= b;
    for (int k = 0; k < 8; ++k)
        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    return crc;
}

struct Crc32Table {
    std::uint32_t v[256];
    constexpr Crc32Table() : v{} {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            v[i] = c;
        }
    }
};
inline constexpr Crc32Table kCrcTable{};

inline std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t crc = 0xFFFF'FFFFu;
    for (const std::uint8_t b : data) crc = kCrcTable.v[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFF'FFFFu;
}

// ---- record framing --------------------------------------------------------
inline constexpr std::size_t kRecordHeaderLen = 4 + 2 + 2 + 4;   // len,type,ver,crc

struct RecordHeader {
    std::uint32_t len = 0;      // total record length (header + payload)
    std::uint16_t type = 0;
    std::uint16_t ver = 0;
    std::uint32_t crc = 0;
};

inline void put_u32le(std::uint8_t* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) { p[i] = static_cast<std::uint8_t>(v); v >>= 8; }
}
inline void put_u16le(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline std::uint32_t get_u32le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint16_t get_u16le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8));
}

/// Serialize a record into out; returns bytes written or 0 (too small/large).
inline std::size_t encode_record(std::span<std::uint8_t> out, RecordType type,
                                 std::uint16_t ver,
                                 std::span<const std::uint8_t> payload) noexcept {
    if (payload.size() > kMaxRecordPayload) return 0;
    const std::size_t total = kRecordHeaderLen + payload.size();
    if (out.size() < total) return 0;
    put_u32le(out.data(), static_cast<std::uint32_t>(total));
    put_u16le(out.data() + 4, static_cast<std::uint16_t>(type));
    put_u16le(out.data() + 6, ver);
    put_u32le(out.data() + 8, crc32(payload));
    std::memcpy(out.data() + kRecordHeaderLen, payload.data(), payload.size());
    return total;
}

enum class ParseStatus : std::uint8_t { Ok, NeedMore, Corrupt };

/// Parse one record at the start of buf. On Ok, advances `*consumed`.
inline ParseStatus parse_record(std::span<const std::uint8_t> buf, RecordHeader& h,
                                std::span<const std::uint8_t>& payload,
                                std::size_t* consumed) noexcept {
    if (buf.size() < kRecordHeaderLen) return ParseStatus::NeedMore;
    const std::uint32_t len = get_u32le(buf.data());
    if (len < kRecordHeaderLen || len > kRecordHeaderLen + kMaxRecordPayload)
        return ParseStatus::Corrupt;
    if (buf.size() < len) return ParseStatus::NeedMore;
    h.len = len;
    h.type = get_u16le(buf.data() + 4);
    h.ver = get_u16le(buf.data() + 6);
    h.crc = get_u32le(buf.data() + 8);
    payload = buf.subspan(kRecordHeaderLen, len - kRecordHeaderLen);
    if (crc32(payload) != h.crc) return ParseStatus::Corrupt;
    *consumed = len;
    return ParseStatus::Ok;
}

// ---- in-memory record (writer ring element) --------------------------------
struct Record {
    std::uint16_t type = 0;
    std::uint16_t ver = 0;
    std::uint16_t len = 0;      // payload length
    std::uint8_t payload[kMaxRecordPayload] = {};
};

/// Engine-side writer. Single producer (engine thread). Two rings:
/// critical (fail-closed on overflow) and telemetry (drop on overflow).
template <std::size_t kRingCapacity = 1024>
class JournalWriter {
public:
    using FailClosedFn = void (*)(void* user);

    void set_fail_closed(FailClosedFn fn, void* user) noexcept {
        fn_ = fn;
        user_ = user;
    }

    /// Critical path: returns false ONLY on ring overflow, after invoking the
    /// fail-closed callback. The record is LOST and the gap is flagged (the
    /// runner writes a Gap marker when the ring drains).
    bool write_critical(RecordType type, std::span<const std::uint8_t> payload) noexcept {
        Record r;
        if (payload.size() > kMaxRecordPayload || !fill(r, type, payload)) {
            flag_gap();
            return false;
        }
        if (!critical_.try_push(r)) {
            flag_gap();
            return false;
        }
        return true;
    }

    /// Telemetry: drop with counter on overflow — never blocks, never fails.
    void write_telemetry(RecordType type, std::span<const std::uint8_t> payload) noexcept {
        Record r;
        if (payload.size() > kMaxRecordPayload || !fill(r, type, payload)) {
            ++telemetry_drops_;
            return;
        }
        if (!telemetry_.try_push(r)) ++telemetry_drops_;
    }

    /// Consumer side (IO thread / test drain): pop one critical record.
    bool pop_critical(Record& out) noexcept { return critical_.try_pop(out); }
    bool pop_telemetry(Record& out) noexcept { return telemetry_.try_pop(out); }
    std::uint64_t telemetry_drops() const noexcept { return telemetry_drops_; }
    std::size_t critical_backlog() const noexcept { return critical_.size_approx(); }
    std::size_t critical_high_water() const noexcept { return critical_.high_water_mark(); }
    bool gap_flagged() const noexcept { return gap_; }
    void clear_gap_flag() noexcept { gap_ = false; }

private:
    static bool fill(Record& r, RecordType type, std::span<const std::uint8_t> p) noexcept {
        r.type = static_cast<std::uint16_t>(type);
        r.ver = 1;
        r.len = static_cast<std::uint16_t>(p.size());
        if (p.size()) std::memcpy(r.payload, p.data(), p.size());
        return true;
    }
    void flag_gap() noexcept {
        gap_ = true;
        if (fn_) fn_(user_);
    }

    core::SpscRing<Record, kRingCapacity> critical_;
    core::SpscRing<Record, kRingCapacity> telemetry_;
    std::uint64_t telemetry_drops_ = 0;
    bool gap_ = false;
    FailClosedFn fn_ = nullptr;
    void* user_ = nullptr;
};

/// File header size: magic[5] | u16 version | u16 reserved.
inline constexpr std::size_t kFileHeaderSize = 9;

// ---- file writer (runs on the IO thread / test drain) ----------------------
class JournalFileWriter {
public:
    bool open(const std::string& path) {
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) return false;
        // header: magic[5] | u16 format_version | u16 reserved = 9 bytes
        std::uint8_t hdr[9] = {};
        std::memcpy(hdr, kMagic, 5);
        put_u16le(hdr + 5, kFormatVersion);
        return std::fwrite(hdr, 1, sizeof(hdr), f_) == sizeof(hdr);
    }

    /// Serialize + write one record. Returns false on IO error.
    bool append(RecordType type, std::uint16_t ver,
                std::span<const std::uint8_t> payload) {
        if (!f_) return false;
        std::uint8_t buf[kRecordHeaderLen + kMaxRecordPayload];
        const std::size_t n = encode_record(buf, type, ver, payload);
        if (n == 0) return false;
        return std::fwrite(buf, 1, n, f_) == n;
    }

    bool append_record(const Record& r) {
        return append(static_cast<RecordType>(r.type), r.ver,
                      std::span<const std::uint8_t>(r.payload, r.len));
    }

    bool flush_ok() {
        return f_ && std::fflush(f_) == 0;
    }
    void close() {
        if (f_) std::fclose(f_);
        f_ = nullptr;
    }
    ~JournalFileWriter() { close(); }

private:
    std::FILE* f_ = nullptr;
};

// ---- reader ----------------------------------------------------------------
class JournalFileReader {
public:
    explicit JournalFileReader(std::vector<std::uint8_t> data) : data_(std::move(data)) {
        if (data_.size() >= kFileHeaderSize &&
            std::memcmp(data_.data(), kMagic, 5) == 0) {
            pos_ = kFileHeaderSize;
            ok_ = true;
        }
    }

    bool ok() const noexcept { return ok_; }
    std::size_t position() const noexcept { return pos_; }

    /// Next record; on Corrupt, stops (last good position kept) and returns
    /// Corrupt — the replay marks a gap at that file offset.
    ParseStatus next(RecordHeader& h, std::span<const std::uint8_t>& payload) noexcept {
        if (!ok_) return ParseStatus::Corrupt;
        const std::size_t before = pos_;
        std::size_t consumed = 0;
        const ParseStatus st =
            parse_record(std::span<const std::uint8_t>(data_).subspan(pos_), h, payload,
                         &consumed);
        if (st == ParseStatus::Ok) pos_ += consumed;
        else pos_ = before;
        return st;
    }

private:
    std::vector<std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool ok_ = false;
};

/// Read a whole file into memory (tools/tests).
inline bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<std::size_t>(n));
    const bool ok = n == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

}  // namespace kestrel::journal
