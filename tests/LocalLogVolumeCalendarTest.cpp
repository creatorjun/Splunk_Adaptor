// tests/LocalLogVolumeCalendarTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/linux/LocalLogVolumeCalendar.hpp"

#include <cstdlib>
#include <ctime>

namespace {

using namespace monitor;
using namespace std::chrono;

struct RestoreTimezone {
    std::optional<std::string> original = std::getenv("TZ")
        ? std::optional<std::string>{std::getenv("TZ")} : std::nullopt;
    ~RestoreTimezone() {
        if (original) ::setenv("TZ", original->c_str(), 1); else ::unsetenv("TZ");
        ::tzset();
    }
};

void interval_tests() {
    CHECK(::setenv("TZ", "KST-9", 1) == 0);
    LocalLogVolumeCalendar calendar;
    LogVolumeSettings settings;
    const auto boundary = sys_days{year{2026}/10/29} + hours{1} + minutes{5};
    auto result = calendar.window(boundary, settings);
    CHECK(result);
    CHECK(result->mode == LogVolumeMode::interval);
    CHECK(result->start_bucket == 202610291000ULL);
    CHECK(result->end_bucket == 202610291005ULL);
    CHECK(result->start == boundary - minutes{5});
    CHECK(result->end == boundary);
    const auto same = calendar.window(boundary + minutes{4} + seconds{59}, settings);
    CHECK(same && *same == *result);
    const auto before = calendar.window(boundary - nanoseconds{1}, settings);
    CHECK(before && before->start_bucket == 202610290955ULL && before->end_bucket == 202610291000ULL);
    settings.interval_seconds = 600;
    result = calendar.window(boundary, settings);
    CHECK(result && result->start_bucket == 202610290950ULL && result->end_bucket == 202610291000ULL);
    settings.interval_seconds = 300;
    CHECK(::setenv("TZ", "UTC0", 1) == 0);
    result = calendar.window(sys_days{year{2026}/1/1}, settings);
    CHECK(result && result->start_bucket == 202512312355ULL && result->end_bucket == 202601010000ULL);
    result = calendar.window(sys_days{year{2024}/3/1}, settings);
    CHECK(result && result->start_bucket == 202402292355ULL && result->end_bucket == 202403010000ULL);
    result = calendar.window(sys_days{year{1970}/1/1} - seconds{1}, settings);
    CHECK(result && result->start_bucket == 196912312350ULL && result->end_bucket == 196912312355ULL);
}

void daily_tests() {
    CHECK(::setenv("TZ", "KST-9", 1) == 0);
    LocalLogVolumeCalendar calendar;
    LogVolumeSettings settings;
    settings.mode = LogVolumeMode::daily;
    const auto due = sys_days{year{2026}/10/28} + hours{15} + minutes{10};
    const auto before = calendar.window(due - seconds{1}, settings);
    CHECK(before && before->start_bucket == 20261027 && before->end_bucket == 20261028);
    auto result = calendar.window(due, settings);
    CHECK(result && result->mode == LogVolumeMode::daily);
    CHECK(result->start_bucket == 20261028 && result->end_bucket == 20261029);
    CHECK(result->start == sys_days{year{2026}/10/27} + hours{15});
    CHECK(result->end == sys_days{year{2026}/10/28} + hours{15});
    CHECK(result->end - result->start == hours{24});
    const auto later = calendar.window(due + hours{23}, settings);
    CHECK(later && *later == *result);
    settings.daily_at_minutes = 23 * 60 + 59;
    result = calendar.window(due, settings);
    CHECK(result && result->start_bucket == 20261027 && result->end_bucket == 20261028);
    result = calendar.window(sys_days{year{2026}/10/29} + hours{14} + minutes{59}, settings);
    CHECK(result && result->start_bucket == 20261028 && result->end_bucket == 20261029);
    CHECK(::setenv("TZ", "UTC0", 1) == 0);
    settings.daily_at_minutes = 10;
    result = calendar.window(sys_days{year{2024}/3/1} + minutes{10}, settings);
    CHECK(result && result->start_bucket == 20240229 && result->end_bucket == 20240301);
    result = calendar.window(sys_days{year{2026}/1/1} + minutes{10}, settings);
    CHECK(result && result->start_bucket == 20251231 && result->end_bucket == 20260101);
    result = calendar.window(sys_days{year{2026}/5/1} + minutes{10}, settings);
    CHECK(result && result->start_bucket == 20260430 && result->end_bucket == 20260501);
}

void daylight_and_invalid_tests() {
    CHECK(::setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1) == 0);
    LocalLogVolumeCalendar calendar;
    LogVolumeSettings settings;
    settings.mode = LogVolumeMode::daily;
    auto result = calendar.window(sys_days{year{2026}/3/9} + hours{4} + minutes{10}, settings);
    CHECK(result && result->start_bucket == 20260308 && result->end_bucket == 20260309);
    CHECK(result->end - result->start == hours{23});
    result = calendar.window(sys_days{year{2026}/11/2} + hours{5} + minutes{10}, settings);
    CHECK(result && result->start_bucket == 20261101 && result->end_bucket == 20261102);
    CHECK(result->end - result->start == hours{25});
    settings.mode = LogVolumeMode::interval;
    result = calendar.window(sys_days{year{2026}/3/8} + hours{7}, settings);
    CHECK(result && result->start_bucket == 202603080155ULL && result->end_bucket == 202603080300ULL);
    result = calendar.window(sys_days{year{2026}/11/1} + hours{6}, settings);
    CHECK(!result && result.error().code == "ambiguous_interval");
    result = calendar.window(sys_days{year{2026}/11/1} + hours{6} + minutes{10}, settings);
    CHECK(!result && result.error().code == "ambiguous_interval");
    for (const unsigned seconds : {0U, 59U, 61U, 86401U}) {
        settings.interval_seconds = seconds;
        CHECK(!calendar.window(system_clock::now(), settings));
    }
    settings.interval_seconds = 300;
    settings.daily_at_minutes = 1440;
    CHECK(!calendar.window(system_clock::now(), settings));
    settings.daily_at_minutes = 10;
    settings.mode = static_cast<LogVolumeMode>(99);
    CHECK(!calendar.window(system_clock::now(), settings));
}

}

int main() {
    return test::run([] {
        RestoreTimezone restore;
        interval_tests();
        daily_tests();
        daylight_and_invalid_tests();
    });
}
