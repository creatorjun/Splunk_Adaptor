// src/infrastructure/database/IMySqlSession.hpp
#pragma once

#include "application/MySqlSettings.hpp"
#include "application/ports/IMySqlCredentialsReader.hpp"
#include "domain/CollectCountAnalysis.hpp"
#include "domain/LogSilenceAnalysis.hpp"
#include "domain/LogVolume.hpp"

#include <expected>
#include <memory>

namespace monitor {

class IMySqlSession {
public:
    virtual ~IMySqlSession() = default;
    virtual std::expected<void, Error> probe() = 0;
    virtual std::expected<std::vector<DailyCountPair>, Error> read_daily_counts(const DailyCountWindow&) {
        return std::unexpected(Error{"collect_count", "CloudESM.SUM_COLLECT_INFO_1DAY", "unsupported",
            "Daily collection count queries are unavailable"});
    }
    virtual std::expected<LogReceiptBatch, Error> read_last_received() {
        return std::unexpected(Error{"log_silence", "CloudESM.SUM_COLLECT_INFO", "unsupported",
            "Last log receipt queries are unavailable"});
    }
    virtual std::expected<std::vector<EquipmentLogVolume>, Error> read_log_volume(const LogVolumeWindow&) {
        return std::unexpected(Error{"log_volume", "query", "unsupported",
            "Windowed log volume queries are unavailable"});
    }
};

class IMySqlConnectionFactory {
public:
    virtual ~IMySqlConnectionFactory() = default;
    virtual std::expected<std::unique_ptr<IMySqlSession>, Error> connect(
        const MySqlSettings& settings, const MySqlCredentials& credentials) = 0;
};

}
