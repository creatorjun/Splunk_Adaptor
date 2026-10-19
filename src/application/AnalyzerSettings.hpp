// src/application/AnalyzerSettings.hpp
#pragma once

namespace monitor {

enum class ComparisonDay { today, yesterday };

struct AnalyzerSettings {
    double collect_count_change_percent{10.0};
    ComparisonDay comparison_day{ComparisonDay::yesterday};

    bool operator==(const AnalyzerSettings&) const = default;
};

}
