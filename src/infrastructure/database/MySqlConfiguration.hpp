// src/infrastructure/database/MySqlConfiguration.hpp
#pragma once

#include "application/ports/IMySqlConfiguration.hpp"
#include "application/ports/ICollectCountReader.hpp"
#include "application/ports/ILogSilenceReader.hpp"
#include "application/ports/ILogVolumeReader.hpp"
#include "infrastructure/database/IMySqlSession.hpp"

#include <chrono>
#include <memory>

namespace monitor {

class MySqlConfiguration final : public IMySqlConfiguration, public ICollectCountReader,
    public ILogSilenceReader, public ILogVolumeReader {
public:
    MySqlConfiguration(IMySqlCredentialsReader& credentials, IMySqlConnectionFactory& factory,
        std::chrono::milliseconds retry_interval = std::chrono::seconds{5},
        std::chrono::milliseconds health_interval = std::chrono::seconds{30});
    ~MySqlConfiguration() override;
    MySqlConfiguration(const MySqlConfiguration&) = delete;
    MySqlConfiguration& operator=(const MySqlConfiguration&) = delete;

    std::expected<bool, Error> prepare(const MySqlSettings& settings) override;
    bool commit() noexcept override;
    CollectCountReading read_daily_counts(const DailyCountWindow& window,
        std::chrono::seconds refresh_interval) override;
    LogSilenceReading read_last_received(bool enabled,
        std::chrono::seconds refresh_interval) override;
    LogVolumeReading read_log_volume(const LogVolumeWindow& window) override;

private:
    class State;
    std::unique_ptr<State> state_;
};

}
