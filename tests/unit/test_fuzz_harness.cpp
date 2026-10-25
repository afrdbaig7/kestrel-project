// Structured-mutation fuzz harness for KITCH/KOUCH decoders (ADR-0002).
//
// Under GCC we run a deterministic seeded campaign that mutates valid frames
// (bit flips, truncations, byte substitutions, splices) and requires the
// decoders to NEVER crash, hang, read out of bounds (checked by ASan builds),
// or return Ok on inconsistent lengths. With Clang + KESTREL_ENABLE_LIBFUZZER
// the same invariants are exercised by real libFuzzer targets in tests/fuzz/.
#include <gtest/gtest.h>
#include <random>

#include "kestrel/protocols/kitch.hpp"
#include "kestrel/protocols/kouch.hpp"

using namespace kestrel::protocols;

namespace {
std::vector<std::uint8_t> valid_kitch(std::mt19937_64& rng) {
    std::uint8_t body[256];
    std::size_t blen = 0;
    kitch::MsgType t = kitch::MsgType::AddOrder;
    switch (rng() % 5) {
        case 0: {
            kitch::AddOrder a{static_cast<std::uint32_t>(rng() % 4),
                              rng(), kitch::kSideBuy,
                              static_cast<std::uint32_t>(rng() % 1000),
                              90'000 + rng() % 20'000};
            blen = kitch::body_add(body, a);
            t = kitch::MsgType::AddOrder;
            break;
        }
        case 1: {
            kitch::OrderCancel c{rng(), static_cast<std::uint32_t>(rng() % 100)};
            blen = kitch::body_cancel(body, c);
            t = kitch::MsgType::OrderCancel;
            break;
        }
        case 2: {
            kitch::OrderExecuted e{rng(), static_cast<std::uint32_t>(rng() % 100), rng()};
            blen = kitch::body_exec(body, e);
            t = kitch::MsgType::OrderExecuted;
            break;
        }
        case 3: {
            kitch::OrderReplace r{rng(), rng(), static_cast<std::uint32_t>(rng() % 100),
                                  90'000 + rng() % 20'000};
            blen = kitch::body_replace(body, r);
            t = kitch::MsgType::OrderReplace;
            break;
        }
        default: {
            kitch::Trade tr{static_cast<std::uint32_t>(rng() % 4),
                            static_cast<std::uint32_t>(rng() % 100),
                            90'000 + rng() % 20'000, rng()};
            blen = kitch::body_trade(body, tr);
            t = kitch::MsgType::Trade;
            break;
        }
    }
    std::vector<std::uint8_t> out(2 + kitch::kHeaderLen + blen);
    const std::size_t n = kitch::encode_framed(out, t, static_cast<std::uint32_t>(rng()),
                                               rng(), {body, blen});
    out.resize(n);
    return out;
}

std::vector<std::uint8_t> valid_kouch(std::mt19937_64& rng) {
    std::vector<std::uint8_t> out;
    switch (rng() % 3) {
        case 0: {
            kouch::Enter e{rng(), static_cast<std::uint32_t>(rng() % 4),
                           (rng() & 1) ? kouch::kSideBuy : kouch::kSideSell,
                           static_cast<std::uint32_t>(rng() % 100),
                           90'000 + rng() % 20'000, kouch::kTifDay};
            out.resize(kouch::kLenEnter);
            kouch::encode_enter(out, e);
            break;
        }
        case 1: {
            kouch::Cancel c{rng()};
            out.resize(kouch::kLenCancel);
            kouch::encode_cancel(out, c);
            break;
        }
        default: {
            kouch::Replace r{rng(), rng(), static_cast<std::uint32_t>(rng() % 100),
                             90'000 + rng() % 20'000};
            out.resize(kouch::kLenReplace);
            kouch::encode_replace(out, r);
            break;
        }
    }
    return out;
}

std::vector<std::uint8_t> mutate(std::mt19937_64& rng, const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> v = in;
    const int nmut = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < nmut && !v.empty(); ++i) {
        switch (rng() % 4) {
            case 0:   // bit flip
                v[rng() % v.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
                break;
            case 1:   // byte substitution
                v[rng() % v.size()] = static_cast<std::uint8_t>(rng());
                break;
            case 2:   // truncate
                v.resize(rng() % (v.size() + 1));
                break;
            default:  // append garbage
                for (int k = 0; k < 4; ++k)
                    v.push_back(static_cast<std::uint8_t>(rng()));
                break;
        }
    }
    return v;
}
}  // namespace

TEST(FuzzHarness, KitchDecoderNeverCrashesAndLengthsConsistent) {
    std::mt19937_64 rng(0xF00D);
    for (int iter = 0; iter < 50'000; ++iter) {
        std::vector<std::uint8_t> f = valid_kitch(rng);
        if (rng() & 1) f = mutate(rng, f);
        // Strip outer framing: feed the payload region (or whatever survives).
        std::span<const std::uint8_t> payload;
        if (f.size() >= 2) {
            const std::size_t plen =
                (static_cast<std::size_t>(f[0]) << 8) | f[1];
            if (f.size() >= 2 + plen) payload = std::span<const std::uint8_t>(f).subspan(2, plen);
            else payload = std::span<const std::uint8_t>(f).subspan(2);
        } else {
            payload = f;
        }
        kitch::Header h{};
        if (kitch::decode_header(payload, h) == kitch::DecodeStatus::Ok) {
            // Length-consistency invariant: Ok implies the exact wire length.
            kitch::AddOrder m{};
            if (kitch::decode_add(payload, m) == kitch::DecodeStatus::Ok) {
                ASSERT_EQ(payload.size(), kitch::kLenAdd);
            }
            kitch::OrderExecuted e{};
            if (kitch::decode_exec(payload, e) == kitch::DecodeStatus::Ok) {
                ASSERT_EQ(payload.size(), kitch::kLenExec);
            }
        }
        // Also feed full frame bytes as payload (malformed on purpose).
        kitch::Header h2{};
        (void)kitch::decode_header(f, h2);
    }
    SUCCEED();
}

TEST(FuzzHarness, KouchDecoderNeverCrashes) {
    std::mt19937_64 rng(0xBEEF);
    for (int iter = 0; iter < 50'000; ++iter) {
        std::vector<std::uint8_t> f = valid_kouch(rng);
        if (rng() & 1) f = mutate(rng, f);
        kouch::Enter e{}; kouch::Cancel c{}; kouch::Replace r{};
        const auto st = kouch::decode_request(f, e, c, r);
        if (st == kouch::ReqDecode::Ok) {
            switch (f[0]) {
                case 'E': EXPECT_EQ(f.size(), kouch::kLenEnter); break;
                case 'C': EXPECT_EQ(f.size(), kouch::kLenCancel); break;
                case 'R': EXPECT_EQ(f.size(), kouch::kLenReplace); break;
                default: FAIL(); break;
            }
        }
        kouch::RespHeader h{};
        (void)kouch::decode_response_header(f, h);
        kouch::Accepted a{}; kouch::Declined d{}; kouch::Replaced rp{};
        kouch::Canceled cx{}; kouch::CancelReject g{}; kouch::Executed ex{};
        (void)kouch::decode_accepted(f, a);
        (void)kouch::decode_declined(f, d);
        (void)kouch::decode_replaced(f, rp);
        (void)kouch::decode_canceled(f, cx);
        (void)kouch::decode_cancel_reject(f, g);
        (void)kouch::decode_executed(f, ex);
    }
    SUCCEED();
}
