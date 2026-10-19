// src/infrastructure/database/MariaDbConnectionFactory.cpp
#include "infrastructure/database/MariaDbConnectionFactory.hpp"
#include "infrastructure/database/CollectCountQuery.hpp"
#include "infrastructure/database/LogSilenceQuery.hpp"

#include <mysql.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace monitor {
namespace {

class ClientLibrary {
public:
    ClientLibrary() {
        if (::mysql_library_init(0, nullptr, nullptr) != 0) {
            throw std::runtime_error("Unable to initialize database client library");
        }
    }
    ~ClientLibrary() { ::mysql_library_end(); }
    ClientLibrary(const ClientLibrary&) = delete;
    ClientLibrary& operator=(const ClientLibrary&) = delete;
};

struct CloseConnection {
    void operator()(MYSQL* connection) const noexcept {
        if (connection) {
            ::mysql_close(connection);
        }
    }
};

struct FreeResult {
    void operator()(MYSQL_RES* result) const noexcept { ::mysql_free_result(result); }
};

using Connection = std::unique_ptr<MYSQL, CloseConnection>;

Error database_error(MYSQL* connection, const char* operation) {
    return {"mysql", "connection", std::to_string(::mysql_errno(connection)), operation};
}

class MariaDbSession final : public IMySqlSession {
public:
    explicit MariaDbSession(Connection connection) : connection_(std::move(connection)) {}

    std::expected<void, Error> probe() override {
        constexpr char query[] = "SELECT 1";
        if (::mysql_real_query(connection_.get(), query, sizeof(query) - 1) != 0) {
            return std::unexpected(database_error(connection_.get(), "Database health check failed"));
        }
        std::unique_ptr<MYSQL_RES, FreeResult> result(::mysql_store_result(connection_.get()));
        if (!result) {
            return std::unexpected(database_error(connection_.get(), "Database health result failed"));
        }
        if (::mysql_num_fields(result.get()) != 1 || ::mysql_num_rows(result.get()) != 1) {
            return std::unexpected(Error{"mysql", "connection", "invalid_probe_result",
                "Database health result was invalid"});
        }
        const auto row = ::mysql_fetch_row(result.get());
        const auto lengths = ::mysql_fetch_lengths(result.get());
        if (!row || !row[0] || !lengths || lengths[0] != 1 || row[0][0] != '1') {
            return std::unexpected(Error{"mysql", "connection", "invalid_probe_result",
                "Database health result was invalid"});
        }
        return {};
    }

    std::expected<std::vector<DailyCountPair>, Error> read_daily_counts(const DailyCountWindow& window) override {
        const auto query = make_collect_count_query(window);
        if (!query) {
            return std::unexpected(query.error());
        }
        if (::mysql_real_query(connection_.get(), query->data(), static_cast<unsigned long>(query->size())) != 0) {
            return std::unexpected(database_error(connection_.get(), "Daily collection count query failed"));
        }
        std::unique_ptr<MYSQL_RES, FreeResult> result(::mysql_use_result(connection_.get()));
        if (!result || ::mysql_num_fields(result.get()) != CollectCountQueryRow{}.size()) {
            return std::unexpected(Error{"collect_count", "CloudESM.SUM_COLLECT_INFO_1DAY", "invalid_query_result",
                "Daily collection count result was invalid"});
        }
        CollectCountQueryResult decoded;
        while (const auto row = ::mysql_fetch_row(result.get())) {
            const auto lengths = ::mysql_fetch_lengths(result.get());
            if (!lengths) {
                return std::unexpected(Error{"collect_count", "CloudESM.SUM_COLLECT_INFO_1DAY", "invalid_query_result",
                    "Daily collection count result was invalid"});
            }
            CollectCountQueryRow fields;
            for (std::size_t index = 0; index < fields.size(); ++index) {
                if (row[index]) {
                    fields[index] = std::string_view{row[index], lengths[index]};
                }
            }
            if (const auto appended = decoded.append(fields); !appended) {
                return std::unexpected(appended.error());
            }
        }
        if (::mysql_errno(connection_.get()) != 0) {
            return std::unexpected(database_error(connection_.get(), "Daily collection count result read failed"));
        }
        return std::move(decoded).take();
    }

    std::expected<LogReceiptBatch, Error> read_last_received() override {
        const auto query = make_log_silence_query();
        if (::mysql_real_query(connection_.get(), query.data(), static_cast<unsigned long>(query.size())) != 0) {
            return std::unexpected(database_error(connection_.get(), "Last log receipt query failed"));
        }
        std::unique_ptr<MYSQL_RES, FreeResult> result(::mysql_use_result(connection_.get()));
        if (!result || ::mysql_num_fields(result.get()) != LogSilenceQueryRow{}.size()) {
            return std::unexpected(Error{"log_silence", "CloudESM.SUM_COLLECT_INFO", "invalid_query_result",
                "Last log receipt result was invalid"});
        }
        LogSilenceQueryResult decoded;
        while (const auto row = ::mysql_fetch_row(result.get())) {
            const auto lengths = ::mysql_fetch_lengths(result.get());
            if (!lengths) {
                return std::unexpected(Error{"log_silence", "CloudESM.SUM_COLLECT_INFO", "invalid_query_result",
                    "Last log receipt result was invalid"});
            }
            LogSilenceQueryRow fields;
            for (std::size_t index = 0; index < fields.size(); ++index) {
                if (row[index]) fields[index] = std::string_view{row[index], lengths[index]};
            }
            if (const auto appended = decoded.append(fields); !appended) {
                return std::unexpected(appended.error());
            }
        }
        if (::mysql_errno(connection_.get()) != 0) {
            return std::unexpected(database_error(connection_.get(), "Last log receipt result read failed"));
        }
        return std::move(decoded).take();
    }

private:
    Connection connection_;
};

}

MariaDbConnectionFactory::MariaDbConnectionFactory() {
    static ClientLibrary library;
}

MariaDbConnectionFactory::~MariaDbConnectionFactory() = default;

std::expected<std::unique_ptr<IMySqlSession>, Error> MariaDbConnectionFactory::connect(
    const MySqlSettings& settings, const MySqlCredentials& credentials) {
    Connection connection(::mysql_init(nullptr));
    if (!connection) {
        return std::unexpected(Error{"mysql", "connection", "initialization_failed",
            "Database connection initialization failed"});
    }
    if (settings.tls_mode != "disabled" && settings.tls_mode != "verify_identity") {
        return std::unexpected(Error{"mysql", "connection", "invalid_tls_mode", "Invalid database TLS mode"});
    }
    if (settings.tls_mode == "verify_identity" && settings.tls_ca_file.empty()) {
        return std::unexpected(Error{"mysql", "connection", "missing_tls_ca", "Database TLS CA file is required"});
    }
    const my_bool reconnect = 0;
    const my_bool enforce_tls = settings.tls_mode == "disabled" ? 0 : 1;
    const my_bool verify_identity = settings.tls_mode == "verify_identity" ? 1 : 0;
    const unsigned int local_infile = 0;
    const unsigned int protocol = MYSQL_PROTOCOL_TCP;
    auto option = [&](mysql_option name, const void* value) {
        return ::mysql_options(connection.get(), name, value) == 0;
    };
    if (!option(MYSQL_OPT_CONNECT_TIMEOUT, &settings.connect_timeout_seconds)
        || !option(MYSQL_OPT_READ_TIMEOUT, &settings.read_timeout_seconds)
        || !option(MYSQL_OPT_WRITE_TIMEOUT, &settings.write_timeout_seconds)
        || !option(MYSQL_OPT_RECONNECT, &reconnect)
        || !option(MYSQL_OPT_PROTOCOL, &protocol)
        || !option(MYSQL_OPT_LOCAL_INFILE, &local_infile)
        || !option(MYSQL_SET_CHARSET_NAME, "utf8mb4")
        || !option(MYSQL_OPT_SSL_ENFORCE, &enforce_tls)
        || !option(MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify_identity)
        || (!settings.tls_ca_file.empty() && settings.tls_mode != "disabled"
            && !option(MYSQL_OPT_SSL_CA, settings.tls_ca_file.c_str()))) {
        return std::unexpected(database_error(connection.get(), "Database connection option failed"));
    }
    if (!::mysql_real_connect(connection.get(), settings.host.c_str(), credentials.username.c_str(),
            credentials.password.c_str(), settings.database.c_str(), settings.port, nullptr, 0)) {
        return std::unexpected(database_error(connection.get(), "Database connection failed"));
    }
    if (settings.tls_mode != "disabled") {
        const auto cipher = ::mysql_get_ssl_cipher(connection.get());
        if (!cipher || !*cipher) {
            return std::unexpected(Error{"mysql", "connection", "tls_required",
                "Database server did not establish the required encrypted connection"});
        }
    }
    return std::make_unique<MariaDbSession>(std::move(connection));
}

}
