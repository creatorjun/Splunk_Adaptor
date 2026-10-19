// src/infrastructure/database/CollectCountQuery.hpp
#pragma once

#include "domain/CollectCountAnalysis.hpp"

#include <array>
#include <set>
#include <string_view>

namespace monitor {

inline constexpr std::size_t collect_count_max_equipment = 10000;
using CollectCountQueryRow = std::array<std::optional<std::string_view>, 9>;

std::expected<std::string, Error> make_collect_count_query(const DailyCountWindow& window);

class CollectCountQueryResult {
public:
    std::expected<void, Error> append(const CollectCountQueryRow& row);
    std::vector<DailyCountPair> take() &&;

private:
    std::vector<DailyCountPair> counts_;
    std::set<std::string> equipment_;
};

}
