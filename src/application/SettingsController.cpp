// src/application/SettingsController.cpp
#include "application/SettingsController.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace monitor {
namespace {

Error not_initialized() {
    return Error{"settings_config", "settings", "not_initialized",
        "Settings controller has not been initialized"};
}

}

SettingsController::SettingsController(ISettingsReader& reader, IOutputDirectory& output,
    SettingsOverrides overrides, IMySqlConfiguration* mysql, IMySqlConfiguration* log_silence_mysql)
    : reader_(reader), output_(output), overrides_(std::move(overrides)), mysql_(mysql),
      log_silence_mysql_(log_silence_mysql) {}

std::expected<void, Error> SettingsController::initialize() {
    if (settings_) {
        return std::unexpected(Error{"settings_config", "settings", "already_initialized",
            "Settings controller has already been initialized"});
    }
    const auto result = load_and_apply();
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

std::expected<bool, Error> SettingsController::reload() {
    if (!settings_) {
        return std::unexpected(not_initialized());
    }
    return load_and_apply();
}

const MonitorSettings& SettingsController::current() const {
    if (!settings_) {
        throw std::logic_error("Settings controller has not been initialized");
    }
    return *settings_;
}

std::expected<ThresholdPolicy, Error> SettingsController::read() {
    if (!settings_) {
        return std::unexpected(not_initialized());
    }
    return settings_->thresholds;
}

std::optional<Error> SettingsController::reload_error() const {
    return last_error_;
}

std::vector<Error> SettingsController::dependency_errors() const {
    return dependency_errors_;
}

std::expected<bool, Error> SettingsController::load_and_apply() {
    auto next = reader_.read_settings();
    if (!next) {
        last_error_ = next.error();
        return std::unexpected(*last_error_);
    }
    if (overrides_.interval) {
        next->collection_interval = *overrides_.interval;
    }
    if (overrides_.output_directory) {
        next->output_directory = *overrides_.output_directory;
    }
    next->mysql.enabled = next->is_analyzer;
    if (!mysql_ && next->mysql.enabled) {
        last_error_ = Error{"mysql", "connection", "unavailable", "Database adapter is unavailable"};
        return std::unexpected(*last_error_);
    }
    const bool changed = !settings_ || *settings_ != *next;
    if (!settings_ || settings_->output_directory != next->output_directory) {
        const auto configured = output_.configure_output(next->output_directory);
        if (!configured) {
            last_error_ = configured.error();
            return std::unexpected(*last_error_);
        }
    }
    bool database_changed{};
    dependency_errors_.clear();
    const std::array databases{mysql_, log_silence_mysql_};
    for (std::size_t index = 0; index < databases.size(); ++index) {
        auto* database = databases[index];
        if (!database) continue;
        const auto prepared = database->prepare(next->mysql);
        if (prepared && *prepared) {
            database_changed = database->commit() || database_changed;
        } else {
            auto error = prepared ? Error{"mysql", "connection", "connection_pending",
                "Database connection validation is pending"} : prepared.error();
            if (log_silence_mysql_ && error.component == "mysql" && error.target == "connection") {
                error.target = index == 0 ? "collect_count.connection" : "log_silence.connection";
            }
            dependency_errors_.push_back(std::move(error));
        }
    }
    settings_ = std::move(*next);
    last_error_.reset();
    return changed || database_changed;
}

}
