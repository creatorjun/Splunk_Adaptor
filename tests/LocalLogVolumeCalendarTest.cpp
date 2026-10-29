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

void boundary_tests() {
    CHECK(::setenv("TZ", "KST-9", 1) == 0);
    LocalLogVolumeCalendar calendar;
    const auto boundary = sys_days{year{2026}/10/29} + hours{1} + minutes{5};
    const auto result = calendar.window(boundary);
    CHECK(result && result->start_bucket == 202610291000ULL && result->end_bucket == 202610291005ULL);
    CHECK(result->start == boundary - minutes{5} && result->end == boundary);
    const auto same = calendar.window(boundary + minutes{4} + seconds{59});
    CHECK(same && *same == *result);
    const auto before = calendar.window(boundary - nanoseconds{1});
    CHECK(before && before->start_bucket == 202610290955ULL && before->end_bucket == 202610291000ULL);
    const auto hour = calendar.hour(boundary);
    CHECK(hour && hour->start == boundary - minutes{5} && hour->end == boundary + minutes{55});
    CHECK(hour->coverage_start == boundary && hour->completed_intervals == 0);
    const auto range = calendar.range(boundary - minutes{10}, boundary + minutes{50});
    CHECK(range && range->start_bucket == 202610290955ULL && range->end_bucket == 202610291055ULL);
    CHECK(!calendar.range(boundary - minutes{15}, boundary + minutes{50}));
    CHECK(!calendar.range(boundary, boundary + minutes{1}));
    CHECK(!calendar.range(boundary + seconds{1}, boundary + minutes{5}));
    CHECK(!calendar.range(boundary, boundary));
    CHECK(::setenv("TZ", "UTC0", 1) == 0);
    const auto midnight = calendar.window(sys_days{year{2026}/1/1});
    CHECK(midnight && midnight->start_bucket == 202512312355ULL && midnight->end_bucket == 202601010000ULL);
    const auto leap = calendar.window(sys_days{year{2024}/3/1});
    CHECK(leap && leap->start_bucket == 202402292355ULL && leap->end_bucket == 202403010000ULL);
    const auto negative = calendar.window(sys_days{year{1970}/1/1} - seconds{1});
    CHECK(negative && negative->start_bucket == 196912312350ULL && negative->end_bucket == 196912312355ULL);
}

void daylight_and_extreme_tests() {
    CHECK(::setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1) == 0);
    LocalLogVolumeCalendar calendar;
    const auto spring = calendar.window(sys_days{year{2026}/3/8} + hours{7});
    CHECK(spring && spring->start_bucket == 202603080155ULL && spring->end_bucket == 202603080300ULL);
    const auto hour = calendar.hour(sys_days{year{2026}/3/8} + hours{6} + minutes{55});
    CHECK(hour && hour->end == sys_days{year{2026}/3/8} + hours{7});
    const auto autumn = calendar.window(sys_days{year{2026}/11/1} + hours{6});
    CHECK(!autumn && autumn.error().code == "ambiguous_interval");
    const auto repeated = calendar.range(sys_days{year{2026}/11/1} + hours{5},
        sys_days{year{2026}/11/1} + hours{6});
    CHECK(!repeated);
    CHECK(!calendar.window(system_clock::time_point::min()));
    CHECK(!calendar.range(system_clock::time_point::min(), system_clock::time_point::max()));
    CHECK(!calendar.range(system_clock::time_point::max(), system_clock::time_point::min()));
    CHECK(::setenv("TZ", "UTC0", 1) == 0);
    CHECK(calendar.window(system_clock::time_point::max()));
}

}

int main() {
    return test::run([] {
        RestoreTimezone restore;
        boundary_tests();
        daylight_and_extreme_tests();
    });
}