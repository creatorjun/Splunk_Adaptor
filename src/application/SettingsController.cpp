// src/application/SettingsController.cpp
#include "application/SettingsController.hpp"

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
    SettingsOverrides overrides, IMySqlConfiguration* mysql)
    : reader_(reader), output_(output), overrides_(std::move(overrides)), mysql_(mysql) {}

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
    next->mysql.enabled = next->mysql_enabled();
    if (!mysql_ && next->mysql.enabled) {
        last_error_ = Error{"mysql", "connection", "unavailable", "Database adapter is unavailable"};
        return std::unexpected(*last_error_);
    }
    if (mysql_) {
        const auto prepared = mysql_->prepare(next->mysql);
        if (!prepared || !*prepared) {
            last_error_ = prepared ? Error{"mysql", "connection", "connection_pending",
                "Database connection validation is pending"} : prepared.error();
            if (!settings_) {
                const auto configured = output_.configure_output(next->output_directory);
                if (!configured) {
                    last_error_ = configured.error();
                    return std::unexpected(*last_error_);
                }
                next->mysql = MySqlSettings{};
                settings_ = std::move(*next);
                return true;
            }
            return std::unexpected(*last_error_);
        }
    }
    const bool changed = !settings_ || *settings_ != *next;
    if (!settings_ || settings_->output_directory != next->output_directory) {
        const auto configured = output_.configure_output(next->output_directory);
        if (!configured) {
            last_error_ = configured.error();
            return std::unexpected(*last_error_);
        }
    }
    const bool database_changed = mysql_ && mysql_->commit();
    settings_ = std::move(*next);
    last_error_.reset();
    return changed || database_changed;
}

}
