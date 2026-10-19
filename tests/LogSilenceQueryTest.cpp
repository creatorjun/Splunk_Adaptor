// tests/LogSilenceQueryTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/database/LogSilenceQuery.hpp"

#include <utility>

namespace {

using namespace monitor;

LogSilenceQueryRow valid_row() {
    return {"192.0.2.1", "202610191230", "1792380600", "1792380900"};
}

bool accepts(const LogSilenceQueryRow& row) {
    LogSilenceQueryResult result;
    return result.append(row).has_value() && std::move(result).take().has_value();
}

void query_tests() {
    const auto query = make_log_silence_query();
    CHECK(query.starts_with("SELECT received.eqp_ip, received.last_bucket"));
    CHECK(query.find("MAX(CASE WHEN `count` > 0 THEN recv_time END)") != std::string::npos);
    CHECK(query.find("FROM CloudESM.SUM_COLLECT_INFO WHERE eqp_ip IS NOT NULL AND eqp_ip <> ''") != std::string::npos);
    CHECK(query.find("STR_TO_DATE(CAST(received.last_bucket AS CHAR), '%Y%m%d%H%i')") != std::string::npos);
    CHECK(query.find("(SELECT UNIX_TIMESTAMP() AS observed_at) AS metadata LEFT JOIN") != std::string::npos);
    CHECK(query.ends_with("ORDER BY received.eqp_ip LIMIT 10001"));
    CHECK(query.find("reg_time") == std::string::npos);
    CHECK(query.find("10.1.43.") == std::string::npos);
    CHECK(query.find(';') == std::string::npos);
}

void decoding_tests() {
    LogSilenceQueryResult result;
    CHECK(result.append(valid_row()));
    auto absent = valid_row();
    absent[0] = "2001:db8::1";
    absent[1].reset();
    absent[2].reset();
    CHECK(result.append(absent));
    auto batch = std::move(result).take();
    CHECK(batch);
    CHECK(batch->records.size() == 2);
    CHECK(batch->records[0].eqp_ip == "192.0.2.1");
    CHECK(batch->records[0].last_received_at.has_value());
    CHECK(!batch->records[1].last_received_at.has_value());
    CHECK(batch->observed_at.time_since_epoch() == std::chrono::seconds{1792380900});
    CHECK(batch->records[0].last_received_at->time_since_epoch() == std::chrono::seconds{1792380600});
    LogSilenceQueryResult empty;
    CHECK(empty.append({std::nullopt, std::nullopt, std::nullopt, "1792380900"}));
    const auto empty_batch = std::move(empty).take();
    CHECK(empty_batch && empty_batch->records.empty());
    CHECK(empty_batch->observed_at == batch->observed_at);
    LogSilenceQueryResult missing;
    CHECK(!std::move(missing).take());
    auto leap = valid_row();
    leap[1] = "202402292359";
    CHECK(accepts(leap));
}

void invalid_data_tests() {
    for (const auto value : {"", "202602291230", "202613011230", "202604311230", "202610192400",
                            "202610191260", "20261019123", "0202610191230", "2026101912300",
                            "20261019123x", "-20261019123", "202610191230 ", "000001011230"}) {
        auto row = valid_row();
        row[1] = value;
        CHECK(!accepts(row));
    }
    for (std::size_t index : {std::size_t{2}, std::size_t{3}}) {
        for (const auto value : {"", "0", "-1", "+1", "1.0", "1x", " 1", "1 ",
                                "18446744073709551616", "9223372037", "253402300799"}) {
            auto row = valid_row();
            row[index] = value;
            CHECK(!accepts(row));
        }
        auto row = valid_row();
        row[index].reset();
        CHECK(!accepts(row));
    }
    for (const auto value : {"", "1234567890123456789012345678901234567890", "192.0.2.1\n"}) {
        auto row = valid_row();
        row[0] = value;
        CHECK(!accepts(row));
    }
    auto row = valid_row();
    row[0].reset();
    CHECK(!accepts(row));
    row = valid_row();
    row[1].reset();
    CHECK(!accepts(row));
    LogSilenceQueryResult duplicates;
    CHECK(duplicates.append(valid_row()));
    CHECK(!duplicates.append(valid_row()));
    LogSilenceQueryResult inconsistent;
    CHECK(inconsistent.append(valid_row()));
    row = valid_row();
    row[0] = "192.0.2.2";
    row[3] = "1792380901";
    CHECK(!inconsistent.append(row));
    LogSilenceQueryResult sentinel_then_row;
    CHECK(sentinel_then_row.append({std::nullopt, std::nullopt, std::nullopt, "1792380900"}));
    CHECK(!sentinel_then_row.append(valid_row()));
    LogSilenceQueryResult row_then_sentinel;
    CHECK(row_then_sentinel.append(valid_row()));
    CHECK(!row_then_sentinel.append({std::nullopt, std::nullopt, std::nullopt, "1792380900"}));
}

void bound_tests() {
    LogSilenceQueryResult result;
    for (std::size_t index = 0; index < log_silence_max_equipment; ++index) {
        auto row = valid_row();
        const auto identifier = "equipment-" + std::to_string(index);
        row[0] = identifier;
        CHECK(result.append(row));
    }
    auto row = valid_row();
    row[0] = "overflow-equipment";
    CHECK(!result.append(row));
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
