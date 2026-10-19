// tests/CollectCountQueryTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/database/CollectCountQuery.hpp"

#include <utility>

namespace {

using namespace monitor;

CollectCountQueryRow valid_row() {
    return {"192.0.2.1", "150", "100", "2", "2", "2", "2", "50", "50"};
}

bool accepts(const CollectCountQueryRow& row) {
    CollectCountQueryResult result;
    return result.append(row).has_value();
}

void query_tests() {
    const auto query = make_collect_count_query({20261019, 20261018});
    CHECK(query);
    CHECK(query->starts_with("SELECT eqp_ip, SUM("));
    CHECK(query->find("FROM CloudESM.SUM_COLLECT_INFO_1DAY WHERE recv_time IN (20261018, 20261019)") != std::string::npos);
    CHECK(query->ends_with("GROUP BY eqp_ip ORDER BY eqp_ip LIMIT 10001"));
    CHECK(query->find("COUNT(CASE WHEN recv_time = 20261019 THEN 1 END)") != std::string::npos);
    CHECK(query->find("COUNT(CASE WHEN recv_time = 20261019 THEN `count` END)") != std::string::npos);
    CHECK(query->find("MIN(CASE WHEN recv_time = 20261018 THEN `count` END)") != std::string::npos);
    CHECK(query->find("CURDATE") == std::string::npos);
    CHECK(query->find("DATE(") == std::string::npos);
    CHECK(query->find(';') == std::string::npos);
    CHECK(make_collect_count_query({20260301, 20260228}));
    CHECK(make_collect_count_query({20240229, 20240228}));
    CHECK(make_collect_count_query({20240301, 20240229}));
    CHECK(make_collect_count_query({20270101, 20261231}));
    CHECK(!make_collect_count_query({20260229, 20260228}));
    CHECK(!make_collect_count_query({20260301, 20260229}));
    CHECK(!make_collect_count_query({20261019, 20261019}));
    CHECK(!make_collect_count_query({20261018, 20261019}));
    CHECK(!make_collect_count_query({20261020, 20261018}));
    CHECK(!make_collect_count_query({20261301, 20261231}));
    CHECK(!make_collect_count_query({0, 0}));
    CHECK(!make_collect_count_query({4294967295U, 20261018}));
}

void decoding_tests() {
    CollectCountQueryResult result;
    CHECK(result.append(valid_row()));
    auto current_only = valid_row();
    current_only[0] = "2001:db8::1";
    current_only[2] = "0";
    current_only[4] = "0";
    current_only[6] = "0";
    current_only[8].reset();
    CHECK(result.append(current_only));
    auto previous_only = valid_row();
    previous_only[0] = "192.0.2.3";
    previous_only[1] = "0";
    previous_only[3] = "0";
    previous_only[5] = "0";
    previous_only[7].reset();
    CHECK(result.append(previous_only));
    auto actual_zero = valid_row();
    actual_zero[0] = "192.0.2.4";
    actual_zero[1] = "0";
    actual_zero[7] = "0";
    CHECK(result.append(actual_zero));
    const auto counts = std::move(result).take();
    CHECK(counts.size() == 4);
    CHECK(counts[0].eqp_ip == "192.0.2.1");
    CHECK(counts[0].current_count == 150);
    CHECK(counts[0].previous_count == 100);
    CHECK(counts[0].current_present && counts[0].previous_present);
    CHECK(counts[1].current_present && !counts[1].previous_present);
    CHECK(counts[1].previous_count == 0);
    CHECK(!counts[2].current_present && counts[2].previous_present);
    CHECK(counts[2].current_count == 0);
    CHECK(counts[3].current_present && counts[3].current_count == 0);

    auto large = valid_row();
    large[1] = "18446744073709551615";
    CHECK(accepts(large));
    CollectCountQueryResult empty;
    CHECK(std::move(empty).take().empty());
}

void invalid_data_tests() {
    auto row = valid_row();
    for (std::size_t index = 0; index < row.size(); ++index) {
        auto missing = row;
        missing[index].reset();
        CHECK(!accepts(missing));
    }
    for (std::size_t index = 1; index < row.size(); ++index) {
        for (const auto invalid : {"", "-1", "+1", "1.0", "1x", " 1", "1 ", "18446744073709551616",
                                  "99999999999999999999999999999999999999999999999999999999999999999"}) {
            auto malformed = row;
            malformed[index] = invalid;
            CHECK(!accepts(malformed));
        }
    }
    row[0] = "";
    CHECK(!accepts(row));
    row[0] = "1234567890123456789012345678901234567890";
    CHECK(!accepts(row));
    row[0] = "192.0.2.1\n";
    CHECK(!accepts(row));
    row[0] = std::string_view{"192.0.2.1\0hidden", 16};
    CHECK(!accepts(row));
    row = valid_row();
    row[5] = "1";
    CHECK(!accepts(row));
    row = valid_row();
    row[6] = "3";
    CHECK(!accepts(row));
    row = valid_row();
    row[7] = "-10";
    CHECK(!accepts(row));
    row = valid_row();
    row[7] = "151";
    CHECK(!accepts(row));
    row = valid_row();
    row[3] = "0";
    row[5] = "0";
    row[7].reset();
    CHECK(!accepts(row));
    row[1] = "0";
    row[7] = "0";
    CHECK(!accepts(row));
    row[7].reset();
    row[2] = "0";
    row[4] = "0";
    row[6] = "0";
    row[8].reset();
    CHECK(!accepts(row));

    CollectCountQueryResult duplicates;
    CHECK(duplicates.append(valid_row()));
    CHECK(!duplicates.append(valid_row()));
}

void bound_tests() {
    CollectCountQueryResult result;
    for (std::size_t index = 0; index < collect_count_max_equipment; ++index) {
        auto row = valid_row();
        const auto equipment = "equipment-" + std::to_string(index);
        row[0] = equipment;
        CHECK(result.append(row));
    }
    auto row = valid_row();
    row[0] = "overflow-equipment";
    const auto overflow = result.append(row);
    CHECK(!overflow);
    CHECK(overflow.error().code == "equipment_limit_exceeded");
}

}

int main() {
    return test::run([] {
        query_tests();
        decoding_tests();
        invalid_data_tests();
        bound_tests();
    });
}
