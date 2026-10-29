// tests/JsonLogCountStoreTest.cpp
#include "TestSupport.hpp"
#include "infrastructure/persistence/JsonLogCountStore.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace monitor;
using namespace std::chrono;
using Json = nlohmann::json;
const auto base = sys_days{year{2026}/10/29};

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        char pattern[] = "/tmp/resource-monitor-log-counts-XXXXXX";
        const auto* created = ::mkdtemp(pattern);
        CHECK(created != nullptr);
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct RestoreTimezone {
    std::optional<std::string> original = std::getenv("TZ")
        ? std::optional<std::string>{std::getenv("TZ")} : std::nullopt;
    ~RestoreTimezone() {
        if (original) ::setenv("TZ", original->c_str(), 1); else ::unsetenv("TZ");
        ::tzset();
    }
};

Json read(const std::filesystem::path& path) {
    std::ifstream stream{path};
    CHECK(stream.good());
    Json value;
    stream >> value;
    return value;
}

void write(const std::filesystem::path& path, const Json& value) {
    std::ofstream stream{path};
    CHECK(stream.good());
    stream << value.dump(2);
}

LogCountState unfinished() {
    return {"test-source", base + minutes{55},
        LogCountHour{base, base + hours{1}, base, 11, {{"192.0.2.1", 110, {}}}}, std::nullopt};
}

LogCountState finished() {
    return {"test-source", base + hours{1}, std::nullopt, base};
}

LogCountReport report() {
    return {"test-source", {base, base + hours{1}, base, 12, {{"192.0.2.1", 120, {}}}},
        base + hours{1} + seconds{1}};
}

void durable_roundtrip_and_replay() {
    TemporaryDirectory directory;
    const auto filename = directory.path / "log_counts/log_cnt_20261029T090000+0900.json";
    {
        JsonLogCountStore store;
        const auto missing = store.load(directory.path.string());
        CHECK(missing && !*missing);
        CHECK(store.commit(directory.path.string(), unfinished(), {}));
        const auto initial = store.load(directory.path.string());
        CHECK(initial && *initial && **initial == unfinished());
        const auto alias = store.load(directory.path.string() + "/");
        CHECK(alias && *alias && **alias == unfinished());
        JsonLogCountStore duplicate;
        CHECK(!duplicate.load(directory.path.string()));
        CHECK(store.commit(directory.path.string(), finished(), {report()}));
        CHECK(store.commit(directory.path.string(), finished(), {report()}));
        const auto document = read(filename);
        CHECK(document["window"]["start"] == "2026-10-29T09:00:00+09:00");
        CHECK(document["window"]["end"] == "2026-10-29T10:00:00+09:00");
        CHECK(document["coverage"]["complete"] == true && document["coverage"]["completed_intervals"] == 12);
        CHECK(document["equipment"].size() == 1 && document["equipment"][0]["count"] == 120);
        struct stat info{};
        CHECK(::stat(filename.c_str(), &info) == 0 && (info.st_mode & 0777) == 0640);
        const auto checkpoint = read(directory.path / "log_counts/.checkpoint.json");
        CHECK(checkpoint["last_saved_hour_epoch"].is_number_integer());
        CHECK(!checkpoint["last_saved_hour_epoch"].is_array());
        auto partial = report();
        partial.hour.coverage_start = base + minutes{55};
        partial.hour.completed_intervals = 1;
        partial.hour.equipment[0].count = 10;
        CHECK(!store.commit(directory.path.string(), finished(), {partial}));
        auto other = finished();
        other.source_identity = "different-source";
        auto other_report = report();
        other_report.source_identity = other.source_identity;
        const auto conflict = store.commit(directory.path.string(), other, {other_report});
        CHECK(!conflict && conflict.error().code == "report_conflict");
        CHECK(read(filename)["equipment"][0]["count"] == 120);
    }
    {
        JsonLogCountStore restarted;
        const auto restored = restarted.load(directory.path.string());
        CHECK(restored && *restored && **restored == finished());
        CHECK(restarted.commit(directory.path.string(), finished(), {}));
        CHECK(read(filename)["equipment"][0]["count"] == 120);
    }
}

void failed_output_preserves_cursor() {
    TemporaryDirectory directory;
    JsonLogCountStore store;
    CHECK(store.commit(directory.path.string(), unfinished(), {}));
    const auto target = directory.path / "outside.json";
    write(target, {{"preserved", true}});
    const auto name = directory.path / "log_counts/log_cnt_20261029T090000+0900.json";
    CHECK(::symlink(target.c_str(), name.c_str()) == 0);
    CHECK(!store.commit(directory.path.string(), finished(), {report()}));
    const auto saved = store.load(directory.path.string());
    CHECK(saved && *saved && **saved == unfinished());
    CHECK(read(target)["preserved"] == true);
}

void retention_and_unrelated_files() {
    TemporaryDirectory directory;
    JsonLogCountStore store;
    CHECK(store.commit(directory.path.string(), finished(), {report()}));
    const auto name = directory.path / "log_counts/log_cnt_20261029T090000+0900.json";
    const auto unknown = directory.path / "log_counts/log_cnt_20261028T090000+0900.json";
    write(unknown, {{"kind", "unrelated"}, {"schema_version", 1}});
    const auto unrelated = directory.path / "log_counts/keep.json";
    write(unrelated, {{"keep", true}});
    const auto outside = directory.path / "outside.json";
    write(outside, read(name));
    const auto linked = directory.path / "log_counts/log_cnt_20261027T090000+0900.json";
    CHECK(::symlink(outside.c_str(), linked.c_str()) == 0);
    auto pruned = store.prune(directory.path.string(), base + hours{1} - seconds{1});
    CHECK(pruned && *pruned == 0);
    pruned = store.prune(directory.path.string(), base + hours{1});
    CHECK(pruned && *pruned == 1 && !std::filesystem::exists(name));
    CHECK(std::filesystem::exists(unknown) && std::filesystem::exists(unrelated) && std::filesystem::is_symlink(linked));
    CHECK(std::filesystem::exists(directory.path / "log_counts/.checkpoint.json"));
    CHECK(std::filesystem::exists(outside));
}

void invalid_checkpoint_is_not_reset() {
    TemporaryDirectory directory;
    JsonLogCountStore store;
    CHECK(store.commit(directory.path.string(), unfinished(), {}));
    const auto path = directory.path / "log_counts/.checkpoint.json";
    auto document = read(path);
    document["next_start_epoch"] = std::numeric_limits<std::uint64_t>::max() - 299;
    write(path, document);
    auto loaded = store.load(directory.path.string());
    CHECK(!loaded && loaded.error().code == "invalid_checkpoint");
    CHECK(read(path)["next_start_epoch"] == document["next_start_epoch"]);
    document["next_start_epoch"] = duration_cast<seconds>((base + minutes{55}).time_since_epoch()).count();
    document["hour"]["equipment"][0]["count"] = -1;
    write(path, document);
    loaded = store.load(directory.path.string());
    CHECK(!loaded && loaded.error().code == "invalid_checkpoint");
    std::ofstream malformed{path};
    malformed << "{";
    malformed.close();
    loaded = store.load(directory.path.string());
    CHECK(!loaded && loaded.error().code == "invalid_document");
}

}

int main() {
    return test::run([] {
        RestoreTimezone restore;
        CHECK(::setenv("TZ", "KST-9", 1) == 0);
        durable_roundtrip_and_replay();
        failed_output_preserves_cursor();
        retention_and_unrelated_files();
        invalid_checkpoint_is_not_reset();
    });
}
