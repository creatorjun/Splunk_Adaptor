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

std::chrono::system_clock::time_point local_timestamp(std::uint64_t bucket, LogVolumeMode mode) {
    const auto day = mode == LogVolumeMode::daily ? bucket : bucket / 10000;
    std::tm local{};
    local.tm_year = static_cast<int>(day / 10000) - 1900;
    local.tm_mon = static_cast<int>(day / 100 % 100) - 1;
    local.tm_mday = static_cast<int>(day % 100);
    if (mode == LogVolumeMode::interval) {
        local.tm_hour = static_cast<int>(bucket / 100 % 100);
        local.tm_min = static_cast<int>(bucket % 100);
    }
    local.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(::mktime(&local));
}

LogVolumeWindow window(LogVolumeMode mode, std::uint64_t start, std::uint64_t end) {
    return {mode, start, end, local_timestamp(start, mode), local_timestamp(end, mode)};
}

LogVolumeQueryRow valid_row() {
    return {"192.0.2.1", "150", "2", "2", "50"};
}

bool accepts(const LogVolumeQueryRow& row) {
    LogVolumeQueryResult result;
    return result.append(row).has_value();
}

void query_tests() {
    ScopedTimezone timezone("UTC0");
    const auto minute = make_log_volume_query(window(LogVolumeMode::interval, 202610291000ULL, 202610291005ULL));
    CHECK(minute);
    CHECK(*minute == "SELECT eqp_ip, SUM(`count`), COUNT(*), COUNT(`count`), MIN(`count`) FROM "
        "CloudESM.SUM_COLLECT_INFO WHERE recv_time >= 202610291000 AND recv_time < 202610291005 "
        "GROUP BY eqp_ip ORDER BY eqp_ip LIMIT 10001");
    const auto daily = make_log_volume_query(window(LogVolumeMode::daily, 20261028, 20261029));
    CHECK(daily);
    CHECK(daily->find("FROM CloudESM.SUM_COLLECT_INFO_1DAY WHERE recv_time >= 20261028 AND recv_time < 20261029") != std::string::npos);
    CHECK(daily->find("COUNT(*)") != std::string::npos);
    CHECK(daily->find("SUM(`count`)") != std::string::npos);
    for (const auto& query : {*minute, *daily}) {
        CHECK(query.find("eqp_ip IS NOT NULL") == std::string::npos);
        CHECK(query.find("eqp_ip <> ''") == std::string::npos);
        CHECK(query.find("reg_time") == std::string::npos);
        CHECK(query.find("NOW(") == std::string::npos);
        CHECK(query.find("DATE(") == std::string::npos);
        CHECK(query.find(';') == std::string::npos);
        CHECK(query.find("10.1.43.") == std::string::npos);
    }
    for (const auto& selected : {
        window(LogVolumeMode::interval, 202610291000ULL, 202610291001ULL),
        window(LogVolumeMode::interval, 202610292355ULL, 202610300000ULL),
        window(LogVolumeMode::interval, 202610291000ULL, 202610301000ULL),
        window(LogVolumeMode::interval, 202412312355ULL, 202501010000ULL),
        window(LogVolumeMode::interval, 202402292355ULL, 202403010000ULL),
        window(LogVolumeMode::daily, 20260228, 20260301),
        window(LogVolumeMode::daily, 20240228, 20240229),
        window(LogVolumeMode::daily, 20240229, 20240301),
        window(LogVolumeMode::daily, 20261231, 20270101)}) {
        CHECK(make_log_volume_query(selected));
    }
}

void invalid_window_tests() {
    ScopedTimezone timezone("UTC0");
    const auto valid = window(LogVolumeMode::interval, 202610291000ULL, 202610291005ULL);
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
    invalid.mode = static_cast<LogVolumeMode>(99);
    CHECK(!make_log_volume_query(invalid));
    invalid = valid;
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
    CHECK(!make_log_volume_query(window(LogVolumeMode::interval, 202610291000ULL, 202610301001ULL)));
    const auto day = window(LogVolumeMode::daily, 20261028, 20261029);
    for (const auto bucket : {0ULL, 2026102ULL, 202610280ULL, 20260229ULL, 20261301ULL, 20261000ULL,
        202610281000ULL, 18446744073709551615ULL}) {
        invalid = day;
        invalid.start_bucket = bucket;
        CHECK(!make_log_volume_query(invalid));
        invalid = day;
        invalid.end_bucket = bucket;
        CHECK(!make_log_volume_query(invalid));
    }
    CHECK(!make_log_volume_query(window(LogVolumeMode::daily, 20261028, 20261030)));
    invalid = day;
    invalid.start += 1h;
    CHECK(!make_log_volume_query(invalid));
    invalid = day;
    invalid.end -= 1min;
    CHECK(!make_log_volume_query(invalid));
}

void timezone_tests() {
    {
        ScopedTimezone timezone("KST-9");
        const auto selected = window(LogVolumeMode::interval, 202610291000ULL, 202610291005ULL);
        CHECK(make_log_volume_query(selected));
        CHECK(selected.start == std::chrono::sys_days(std::chrono::year{2026}/10/29) + 1h);
        auto wrong_timezone = selected;
        wrong_timezone.start += 9h;
        wrong_timezone.end += 9h;
        CHECK(!make_log_volume_query(wrong_timezone));
    }
    {
        ScopedTimezone timezone("EST5EDT,M3.2.0/2,M11.1.0/2");
        const auto spring = window(LogVolumeMode::daily, 20260308, 20260309);
        CHECK(spring.end - spring.start == 23h);
        CHECK(make_log_volume_query(spring));
        const auto autumn = window(LogVolumeMode::daily, 20261101, 20261102);
        CHECK(autumn.end - autumn.start == 25h);
        CHECK(make_log_volume_query(autumn));
    }
}

void decoding_tests() {
    LogVolumeQueryResult result;
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
    CHECK(counts[1].eqp_ip == "2001:db8::1" && counts[1].count == 0);
    CHECK(counts[2].count == std::numeric_limits<std::uint64_t>::max());
    CHECK(counts[3].count == 150);
    LogVolumeQueryResult empty;
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
    LogVolumeQueryResult duplicates;
    CHECK(duplicates.append(valid_row()));
    CHECK(!duplicates.append(valid_row()));
}

void bound_tests() {
    LogVolumeQueryResult result;
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
