// src/infrastructure/linux/LocalDayWindow.cpp
#include "infrastructure/linux/LocalDayWindow.hpp"

#include <ctime>

namespace monitor {

std::expected<DailyCountWindow, Error> LocalDayWindow::make(
    std::chrono::system_clock::time_point timestamp, ComparisonDay day) const {
    const auto time = std::chrono::system_clock::to_time_t(timestamp);
    ::tzset();
    std::tm local{};
    const auto error = Error{"collect_count", "calendar", "invalid_date", "Unable to determine local comparison dates"};
    if (::localtime_r(&time, &local) == nullptr || local.tm_year < -1899 || local.tm_year > 8099) {
        return std::unexpected(error);
    }
    using namespace std::chrono;
    const year_month_day today{year{local.tm_year + 1900}, month{static_cast<unsigned>(local.tm_mon + 1)},
        std::chrono::day{static_cast<unsigned>(local.tm_mday)}};
    if (!today.ok()) return std::unexpected(error);
    const auto current = sys_days{today} - days{day == ComparisonDay::today ? 0 : 1};
    const year_month_day current_date{current};
    const year_month_day previous_date{current - days{1}};
    if (int(previous_date.year()) < 1000 || int(current_date.year()) > 9999) return std::unexpected(error);
    const auto encode = [](year_month_day value) {
        return static_cast<std::uint32_t>(int(value.year())) * 10000U
            + unsigned(value.month()) * 100U + unsigned(value.day());
    };
    return DailyCountWindow{encode(current_date), encode(previous_date)};
}

}
