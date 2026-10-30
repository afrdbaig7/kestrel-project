// Unit tests: journal — record framing, CRC corruption detection, writer
// policies (critical fail-closed / telemetry drop), file round-trip
#include <gtest/gtest.h>
#include <cstring>

#include "kestrel/journal/journal.hpp"

using namespace kestrel::journal;

TEST(Journal, RecordRoundTripAndCrc) {
    const std::uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::uint8_t buf[64];
    const std::size_t n = encode_record(buf, RecordType::Md, 1, payload);
    ASSERT_EQ(n, kRecordHeaderLen + 8);

    RecordHeader h{};
    std::span<const std::uint8_t> pl;
    std::size_t consumed = 0;
    ASSERT_EQ(parse_record(buf, h, pl, &consumed), ParseStatus::Ok);
    EXPECT_EQ(consumed, n);
    EXPECT_EQ(h.type, static_cast<std::uint16_t>(RecordType::Md));
    EXPECT_EQ(pl.size(), 8u);
    EXPECT_EQ(std::memcmp(pl.data(), payload, 8), 0);
}

TEST(Journal, CorruptionDetected) {
    const std::uint8_t payload[] = {1, 2, 3, 4};
    std::uint8_t buf[64];
    const std::size_t n = encode_record(buf, RecordType::Tx, 1, payload);
    // Flip a payload byte -> crc mismatch
    buf[kRecordHeaderLen + 1] ^= 0xFF;
    RecordHeader h{};
    std::span<const std::uint8_t> pl;
    std::size_t consumed = 0;
    EXPECT_EQ(parse_record({buf, n}, h, pl, &consumed), ParseStatus::Corrupt);
    // Restore; flip a header byte (crc field) -> mismatch
    buf[kRecordHeaderLen + 1] ^= 0xFF;
    buf[8] ^= 0x01;
    EXPECT_EQ(parse_record({buf, n}, h, pl, &consumed), ParseStatus::Corrupt);
}

TEST(Journal, TruncatedTailNeedsMore) {
    const std::uint8_t payload[] = {9, 9, 9, 9, 9};
    std::uint8_t buf[64];
    const std::size_t n = encode_record(buf, RecordType::Timer, 1, payload);
    RecordHeader h{};
    std::span<const std::uint8_t> pl;
    std::size_t consumed = 0;
    EXPECT_EQ(parse_record({buf, n - 1}, h, pl, &consumed), ParseStatus::NeedMore);
    // Absurd length prefix -> corrupt (bounds validation)
    put_u32le(buf, 0xFFFFFF);
    EXPECT_EQ(parse_record({buf, n}, h, pl, &consumed), ParseStatus::Corrupt);
}

TEST(JournalWriter, CriticalPathRingAndTelemetryDrops) {
    JournalWriter<4> w;   // tiny rings
    Record r;
    // Fill critical ring (4).
    for (int i = 0; i < 4; ++i) {
        const std::uint8_t b = static_cast<std::uint8_t>(i);
        EXPECT_TRUE(w.write_critical(RecordType::Md, {&b, 1}));
    }
    // Overflow: fail-closed flag + callback.
    bool called = false;
    w.set_fail_closed([](void* user) { *static_cast<bool*>(user) = true; }, &called);
    const std::uint8_t b = 99;
    EXPECT_FALSE(w.write_critical(RecordType::Md, {&b, 1}));
    EXPECT_TRUE(called);
    EXPECT_TRUE(w.gap_flagged());

    // Drain and verify order/content.
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(w.pop_critical(r));
        EXPECT_EQ(r.len, 1u);
        EXPECT_EQ(r.payload[0], i);
    }
    EXPECT_FALSE(w.pop_critical(r));

    // Telemetry overflows drop with counter, never fail.
    for (int i = 0; i < 10; ++i) {
        const std::uint8_t tb = static_cast<std::uint8_t>(i);
        w.write_telemetry(RecordType::Marker, {&tb, 1});
    }
    EXPECT_EQ(w.telemetry_drops(), 6u);
    int drained = 0;
    while (w.pop_telemetry(r)) ++drained;
    EXPECT_EQ(drained, 4);
}

TEST(Journal, FileRoundTrip) {
    const std::string path = "/tmp/kestrel_test_journal.bin";
    {
        JournalFileWriter w;
        ASSERT_TRUE(w.open(path));
        const std::uint8_t p1[] = {1, 2, 3};
        const std::uint8_t p2[] = {4, 5};
        EXPECT_TRUE(w.append(RecordType::Md, 1, p1));
        EXPECT_TRUE(w.append(RecordType::Exec, 1, p2));
        EXPECT_TRUE(w.append(RecordType::Tx, 1, p1));
        ASSERT_TRUE(w.flush_ok());
        w.close();
    }
    std::vector<std::uint8_t> data;
    ASSERT_TRUE(read_file(path, data));
    JournalFileReader r(std::move(data));
    ASSERT_TRUE(r.ok());
    RecordHeader h{};
    std::span<const std::uint8_t> pl;
    ASSERT_EQ(r.next(h, pl), ParseStatus::Ok);
    EXPECT_EQ(h.type, static_cast<std::uint16_t>(RecordType::Md));
    EXPECT_EQ(pl.size(), 3u);
    ASSERT_EQ(r.next(h, pl), ParseStatus::Ok);
    EXPECT_EQ(h.type, static_cast<std::uint16_t>(RecordType::Exec));
    ASSERT_EQ(r.next(h, pl), ParseStatus::Ok);
    EXPECT_EQ(h.type, static_cast<std::uint16_t>(RecordType::Tx));
    EXPECT_EQ(r.next(h, pl), ParseStatus::NeedMore);   // clean EOF
}

TEST(Journal, FileCorruptionStopsAtLastGood) {
    const std::string path = "/tmp/kestrel_test_journal_corrupt.bin";
    {
        JournalFileWriter w;
        ASSERT_TRUE(w.open(path));
        const std::uint8_t p1[] = {1};
        EXPECT_TRUE(w.append(RecordType::Md, 1, p1));
        EXPECT_TRUE(w.append(RecordType::Tx, 1, p1));
        ASSERT_TRUE(w.flush_ok());
        w.close();
        // Corrupt the SECOND record's payload byte in place.
        // Record 1 (payload {1}) occupies [8, 8+12+1); record 2 payload starts
        // at 8 + 13 + 12.
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        std::fseek(f, kFileHeaderSize + 13 + kRecordHeaderLen, SEEK_SET);
        const std::uint8_t bad = 0xEE;
        std::fwrite(&bad, 1, 1, f);
        std::fclose(f);
    }
    std::vector<std::uint8_t> data;
    ASSERT_TRUE(read_file(path, data));
    JournalFileReader r(std::move(data));
    RecordHeader h{};
    std::span<const std::uint8_t> pl;
    ASSERT_EQ(r.next(h, pl), ParseStatus::Ok);
    EXPECT_EQ(h.type, static_cast<std::uint16_t>(RecordType::Md));
    EXPECT_EQ(r.next(h, pl), ParseStatus::Corrupt);
    EXPECT_EQ(r.position(), kFileHeaderSize + 13);   // stopped after last good
}

TEST(Journal, PayloadTooLargeRejected) {
    std::vector<std::uint8_t> big(kMaxRecordPayload + 1, 7);
    std::uint8_t buf[kRecordHeaderLen + kMaxRecordPayload + 16];
    EXPECT_EQ(encode_record(buf, RecordType::Snapshot, 1, big), 0u);
}
