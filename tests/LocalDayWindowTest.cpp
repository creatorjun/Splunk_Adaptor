// tests/LocalDayWindowTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/linux/LocalDayWindow.hpp"

#include <cstdlib>
#include <ctime>

int main() {
    return test::run([] {
        using namespace std::chrono;
        monitor::LocalDayWindow calendar;
        const auto original = std::getenv("TZ") ? std::optional<std::string>{std::getenv("TZ")} : std::nullopt;
        struct Restore {
            std::optional<std::string> zone;
            ~Restore() {
                if (zone) ::setenv("TZ", zone->c_str(), 1); else ::unsetenv("TZ");
                ::tzset();
            }
        } restore{original};
        CHECK(::setenv("TZ", "KST-9", 1) == 0);
        const auto utc = sys_days{year{2026}/10/18} + hours{16};
        auto result = calendar.make(utc, monitor::ComparisonDay::today);
        CHECK(result && result->current_day == 20261019 && result->previous_day == 20261018);
        result = calendar.make(utc, monitor::ComparisonDay::yesterday);
        CHECK(result && result->current_day == 20261018 && result->previous_day == 20261017);
        CHECK(::setenv("TZ", "UTC0", 1) == 0);
        result = calendar.make(utc, monitor::ComparisonDay::today);
        CHECK(result && result->current_day == 20261018 && result->previous_day == 20261017);
        result = calendar.make(sys_days{year{2024}/3/1}, monitor::ComparisonDay::yesterday);
        CHECK(result && result->current_day == 20240229 && result->previous_day == 20240228);
        result = calendar.make(sys_days{year{2026}/1/1}, monitor::ComparisonDay::today);
        CHECK(result && result->previous_day == 20251231);
        CHECK(::setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1) == 0);
        result = calendar.make(sys_days{year{2026}/3/9} + hours{4}, monitor::ComparisonDay::yesterday);
        CHECK(result && result->current_day == 20260308 && result->previous_day == 20260307);
        result = calendar.make(sys_days{year{2026}/11/2} + hours{5}, monitor::ComparisonDay::yesterday);
        CHECK(result && result->current_day == 20261101 && result->previous_day == 20261031);
    });
}
