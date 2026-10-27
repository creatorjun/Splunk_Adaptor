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
    CHECK(query == make_log_silence_query(false));
    const auto indexed = make_log_silence_query(true);
    CHECK(indexed.starts_with("SELECT received.eqp_ip, received.last_bucket"));
    CHECK(indexed.find("SELECT DISTINCT eqp_ip FROM CloudESM.SUM_COLLECT_INFO") != std::string::npos);
    CHECK(indexed.find("WHERE eqp_ip IS NOT NULL AND eqp_ip <> '' ORDER BY eqp_ip LIMIT 10001") != std::string::npos);
    CHECK(indexed.find("recent.eqp_ip = equipment.eqp_ip AND recent.`count` > 0") != std::string::npos);
    CHECK(indexed.find("ORDER BY recent.recv_time DESC LIMIT 1") != std::string::npos);
    CHECK(indexed.find("ORDER BY equipment.eqp_ip LIMIT 10001") != std::string::npos);
    CHECK(indexed.find("STR_TO_DATE(CAST(received.last_bucket AS CHAR), '%Y%m%d%H%i')") != std::string::npos);
    CHECK(indexed.find("(SELECT UNIX_TIMESTAMP() AS observed_at) AS metadata LEFT JOIN") != std::string::npos);
    CHECK(indexed.ends_with("ORDER BY received.eqp_ip LIMIT 10001"));
    CHECK(indexed.find("MAX(") == std::string::npos);
    CHECK(indexed.find("reg_time") == std::string::npos);
    CHECK(indexed.find("10.1.43.") == std::string::npos);
    CHECK(indexed.find(';') == std::string::npos);
}

LogSilenceIndexRow index_row(std::string_view sequence, std::string_view column,
    std::string_view name = "arbitrary_index_name") {
    return {name, sequence, column, std::nullopt, "BTREE", std::nullopt, std::nullopt};
}

void index_columns_tests() {
    const std::array<std::string_view, 14> standard{"Table", "Non_unique", "Key_name", "Seq_in_index",
        "Column_name", "Collation", "Cardinality", "Sub_part", "Packed", "Null", "Index_type",
        "Comment", "Index_comment", "Ignored"};
    const auto maria = find_log_silence_index_columns(standard);
    CHECK(maria && maria->required == (std::array<std::size_t, 5>{2, 3, 4, 7, 10}));
    CHECK(maria->ignored == 13 && !maria->visible);
    const std::array<std::string_view, 8> reordered{"Extra", "visible", "index_type", "sub_part",
        "column_name", "seq_in_index", "key_name", "Expression"};
    const auto mysql = find_log_silence_index_columns(reordered);
    CHECK(mysql && mysql->required == (std::array<std::size_t, 5>{6, 5, 4, 3, 2}));
    CHECK(!mysql->ignored && mysql->visible == 1);
    CHECK(!find_log_silence_index_columns({}));
    auto missing = standard;
    missing[4] = "unexpected";
    CHECK(!find_log_silence_index_columns(missing));
    auto duplicate = standard;
    duplicate[1] = "Key_name";
    CHECK(!find_log_silence_index_columns(duplicate));
    duplicate = standard;
    duplicate[1] = "Ignored";
    CHECK(!find_log_silence_index_columns(duplicate));
}

void index_catalog_tests() {
    LogSilenceIndexCatalog catalog;
    CHECK(!catalog.supports_latest_lookup());
    CHECK(catalog.append(index_row("3", "count")));
    CHECK(catalog.append(index_row("1", "eqp_ip")));
    CHECK(!catalog.supports_latest_lookup());
    CHECK(catalog.append(index_row("2", "recv_time")));
    CHECK(catalog.supports_latest_lookup());
    CHECK(catalog.append(index_row("4", "log_type_nm")));
    CHECK(catalog.supports_latest_lookup());
    CHECK(catalog.append(index_row("1", "recv_time", "unrelated_index")));
    CHECK(catalog.supports_latest_lookup());
    CHECK(!catalog.append(index_row("1", "eqp_ip")));
    for (std::size_t selected = 0; selected < 3; ++selected) {
        LogSilenceIndexCatalog prefixed;
        auto first = index_row("1", "eqp_ip");
        auto second = index_row("2", "recv_time");
        auto third = index_row("3", "count");
        std::array rows{first, second, third};
        rows[selected][3] = "12";
        for (const auto& row : rows) CHECK(prefixed.append(row));
        CHECK(!prefixed.supports_latest_lookup());
    }
    for (const auto kind : {"HASH", "FULLTEXT"}) {
        LogSilenceIndexCatalog wrong_type;
        auto first = index_row("1", "eqp_ip");
        first[4] = kind;
        CHECK(wrong_type.append(first));
        CHECK(wrong_type.append(index_row("2", "recv_time")));
        CHECK(wrong_type.append(index_row("3", "count")));
        CHECK(!wrong_type.supports_latest_lookup());
    }
    for (const auto selected : {std::size_t{5}, std::size_t{6}}) {
        LogSilenceIndexCatalog disabled;
        auto first = index_row("1", "eqp_ip");
        first[selected] = selected == 5 ? "YES" : "NO";
        CHECK(disabled.append(first));
        CHECK(disabled.append(index_row("2", "recv_time")));
        CHECK(disabled.append(index_row("3", "count")));
        CHECK(!disabled.supports_latest_lookup());
    }
    LogSilenceIndexCatalog wrong_order;
    CHECK(wrong_order.append(index_row("1", "recv_time")));
    CHECK(wrong_order.append(index_row("2", "eqp_ip")));
    CHECK(wrong_order.append(index_row("3", "count")));
    CHECK(!wrong_order.supports_latest_lookup());
    LogSilenceIndexCatalog gap;
    CHECK(gap.append(index_row("1", "eqp_ip")));
    CHECK(gap.append(index_row("3", "count")));
    CHECK(gap.append(index_row("4", "other")));
    CHECK(!gap.supports_latest_lookup());
    LogSilenceIndexCatalog functional;
    auto expression = index_row("1", "eqp_ip");
    expression[2].reset();
    CHECK(functional.append(expression));
    CHECK(functional.append(index_row("2", "recv_time")));
    CHECK(functional.append(index_row("3", "count")));
    CHECK(!functional.supports_latest_lookup());
    LogSilenceIndexCatalog visible;
    auto first = index_row("1", "EQP_IP");
    first[4] = "btree";
    first[5] = "NO";
    first[6] = "YES";
    CHECK(visible.append(first));
    CHECK(visible.append(index_row("2", "recv_time")));
    CHECK(visible.append(index_row("3", "count")));
    CHECK(visible.supports_latest_lookup());
    for (const auto sequence : {"", "0", "-1", "1x", "1025", "18446744073709551616"}) {
        LogSilenceIndexCatalog invalid;
        CHECK(!invalid.append(index_row(sequence, "eqp_ip")));
    }
    for (const auto selected : {std::size_t{0}, std::size_t{1}, std::size_t{4}}) {
        LogSilenceIndexCatalog invalid;
        auto row = index_row("1", "eqp_ip");
        row[selected].reset();
        CHECK(!invalid.append(row));
    }
    for (const auto selected : {std::size_t{5}, std::size_t{6}}) {
        LogSilenceIndexCatalog invalid;
        auto row = index_row("1", "eqp_ip");
        row[selected] = "UNKNOWN";
        CHECK(!invalid.append(row));
    }
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
        index_columns_tests();
        index_catalog_tests();
        decoding_tests();
        invalid_data_tests();
        bound_tests();
    });
}
