// tests/LogVolumeQueryTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/database/LogVolumeQuery.hpp"

#include <cstdlib>
#include <ctime>
#include <limits>
#include <utility>

namespace {

using namespace monitor;
using namespace std::chrono_literals;

class ScopedTimezone {
public:
    explicit ScopedTimezone(const char* zone) {
        if (const auto* previous = std::getenv("TZ")) previous_ = previous;
        CHECK(::setenv("TZ", zone, 1) == 0);
        ::tzset();
    }
    ~ScopedTimezone() {
        if (previous_) ::setenv("TZ", previous_->c_str(), 1);
        else ::unsetenv("TZ");
        ::tzset();
    }
    ScopedTimezone(const ScopedTimezone&) = delete;
    ScopedTimezone& operator=(const ScopedTimezone&) = delete;

private:
    std::optional<std::string> previous_;
};

std::chrono::system_clock::time_point local_timestamp(std::uint64_t bucket) {
    const auto day = bucket / 10000;
    std::tm local{};
    local.tm_year = static_cast<int>(day / 10000) - 1900;
    local.tm_mon = static_cast<int>(day / 100 % 100) - 1;
    local.tm_mday = static_cast<int>(day % 100);
    local.tm_hour = static_cast<int>(bucket / 100 % 100);
    local.tm_min = static_cast<int>(bucket % 100);
    local.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(::mktime(&local));
}

LogVolumeWindow window(std::uint64_t start = 202610291000ULL, std::uint64_t end = 202610291100ULL) {
    return {start, end, local_timestamp(start), local_timestamp(end)};
}

LogVolumeQueryRow valid_row() {
    return {"192.0.2.1", "150", "2", "2", "50", "202610291000"};
}

bool accepts(const LogVolumeQueryRow& row) {
    LogVolumeQueryResult result(window());
    return result.append(row).has_value();
}

void query_tests() {
    ScopedTimezone timezone("UTC0");
    const auto query = make_log_volume_query(window(202610291000ULL, 202610291005ULL));
    CHECK(query);
    CHECK(*query == "SELECT eqp_ip, SUM(`count`), COUNT(*), COUNT(`count`), MIN(`count`), "
        "recv_time - MOD(recv_time, 5) AS interval_start FROM CloudESM.SUM_COLLECT_INFO "
        "WHERE recv_time >= 202610291000 AND recv_time < 202610291005 "
        "GROUP BY eqp_ip, interval_start ORDER BY interval_start, eqp_ip LIMIT 120001");
    CHECK(query->find("reg_time") == std::string::npos);
    CHECK(query->find("WHERE DATE(") == std::string::npos);
    CHECK(query->find(';') == std::string::npos);
    for (const auto& selected : {
        window(202610291000ULL, 202610291100ULL),
        window(202610292355ULL, 202610300000ULL),
        window(202412312355ULL, 202501010000ULL),
        window(202402292355ULL, 202403010000ULL)}) {
        CHECK(make_log_volume_query(selected));
    }
}

void invalid_window_tests() {
    ScopedTimezone timezone("UTC0");
    const auto valid = window(202610291000ULL, 202610291005ULL);
    for (const auto bucket : {0ULL, 20261029100ULL, 2026102910000ULL, 202602291000ULL,
        202613011000ULL, 202610001000ULL, 202610292400ULL, 202610291060ULL,
        18446744073709551615ULL}) {
        auto invalid = valid;
        invalid.start_bucket = bucket;
        CHECK(!make_log_volume_query(invalid));
        invalid = valid;
        invalid.end_bucket = bucket;
        CHECK(!make_log_volume_query(invalid));
    }
    auto invalid = valid;
    invalid.end_bucket = invalid.start_bucket;
    CHECK(!make_log_volume_query(invalid));
    invalid = valid;
    std::swap(invalid.start_bucket, invalid.end_bucket);
    CHECK(!make_log_volume_query(invalid));
    invalid = valid;
    invalid.start = invalid.end;
    CHECK(!make_log_volume_query(invalid));
    invalid = valid;
    invalid.start += 1ns;
    CHECK(!make_log_volume_query(invalid));
    invalid = valid;
    invalid.end += 1s;
    CHECK(!make_log_volume_query(invalid));
    invalid = valid;
    invalid.start -= 1min;
    CHECK(!make_log_volume_query(invalid));
    invalid = valid;
    invalid.start = std::chrono::system_clock::time_point::min();
    invalid.end = std::chrono::system_clock::time_point::max();
    CHECK(!make_log_volume_query(invalid));
    CHECK(!make_log_volume_query(window(202610291000ULL, 202610301001ULL)));
    CHECK(!make_log_volume_query(window(202610291000ULL, 202610291001ULL)));
    CHECK(!make_log_volume_query(window(202610291000ULL, 202610291105ULL)));
    CHECK(!make_log_volume_query(window(202610291001ULL, 202610291005ULL)));
}

void timezone_tests() {
    {
        ScopedTimezone timezone("KST-9");
        const auto selected = window(202610291000ULL, 202610291005ULL);
        CHECK(make_log_volume_query(selected));
        CHECK(selected.start == std::chrono::sys_days(std::chrono::year{2026}/10/29) + 1h);
        auto wrong_timezone = selected;
        wrong_timezone.start += 9h;
        wrong_timezone.end += 9h;
        CHECK(!make_log_volume_query(wrong_timezone));
    }

}

void decoding_tests() {
    LogVolumeQueryResult result(window());
    CHECK(result.append(valid_row()));
    auto zero = valid_row();
    zero[0] = "2001:db8::1";
    zero[1] = "0";
    zero[4] = "0";
    CHECK(result.append(zero));
    auto large = valid_row();
    large[0] = "192.0.2.3";
    large[1] = "18446744073709551615";
    large[2] = "3";
    large[3] = "3";
    large[4] = "1";
    CHECK(result.append(large));
    auto single = valid_row();
    single[0] = "192.0.2.4";
    single[2] = "1";
    single[3] = "1";
    single[4] = single[1];
    CHECK(result.append(single));
    const auto counts = std::move(result).take();
    CHECK(counts.size() == 4);
    CHECK(counts[0].eqp_ip == "192.0.2.1" && counts[0].count == 150);
    CHECK(counts[0].start_bucket == 202610291000ULL);
    CHECK(counts[1].eqp_ip == "2001:db8::1" && counts[1].count == 0);
    CHECK(counts[2].count == std::numeric_limits<std::uint64_t>::max());
    CHECK(counts[3].count == 150);
    LogVolumeQueryResult empty(window());
    CHECK(std::move(empty).take().empty());
}

void invalid_data_tests() {
    for (std::size_t index = 0; index < LogVolumeQueryRow{}.size(); ++index) {
        auto row = valid_row();
        row[index].reset();
        CHECK(!accepts(row));
    }
    for (std::size_t index = 1; index < LogVolumeQueryRow{}.size(); ++index) {
        for (const auto invalid : {"", "-1", "+1", "1.0", "1e2", "1x", " 1", "1 ",
            "18446744073709551616", "999999999999999999999999999999999999999999999"}) {
            auto row = valid_row();
            row[index] = invalid;
            CHECK(!accepts(row));
        }
    }
    for (const auto invalid : {"", "1234567890123456789012345678901234567890", "192.0.2.1\n", "192.0.2.1\177"}) {
        auto row = valid_row();
        row[0] = invalid;
        CHECK(!accepts(row));
    }
    auto row = valid_row();
    row[0] = std::string_view{"192.0.2.1\0hidden", 15};
    CHECK(!accepts(row));
    row = valid_row();
    row[2] = "0";
    row[3] = "0";
    CHECK(!accepts(row));
    row = valid_row();
    row[3] = "1";
    CHECK(!accepts(row));
    row[3] = "3";
    CHECK(!accepts(row));
    row = valid_row();
    row[4] = "151";
    CHECK(!accepts(row));
    row[4] = "76";
    CHECK(!accepts(row));
    row = valid_row();
    row[2] = "1";
    row[3] = "1";
    CHECK(!accepts(row));
    row = valid_row();
    row[1] = "0";
    CHECK(!accepts(row));
    LogVolumeQueryResult duplicates(window());
    CHECK(duplicates.append(valid_row()));
    CHECK(!duplicates.append(valid_row()));
    auto other_interval = valid_row();
    other_interval[5] = "202610291005";
    CHECK(duplicates.append(other_interval));
    for (const auto invalid : {"202610290955", "202610291100", "202610291001", "202610291060"}) {
        auto wrong_bucket = valid_row();
        wrong_bucket[5] = invalid;
        CHECK(!accepts(wrong_bucket));
    }
}

void bound_tests() {
    LogVolumeQueryResult result(window());
    for (std::size_t index = 0; index < log_volume_max_equipment; ++index) {
        auto row = valid_row();
        const auto equipment = "equipment-" + std::to_string(index);
        row[0] = equipment;
        CHECK(result.append(row));
    }
    auto row = valid_row();
    row[0] = "overflow-equipment";
    const auto overflow = result.append(row);
    CHECK(!overflow && overflow.error().code == "equipment_limit_exceeded");
}

}

int main() {
    return test::run([] {
        query_tests();
        invalid_window_tests();
        timezone_tests();
        decoding_tests();
        invalid_data_tests();
        bound_tests();
    });
}
