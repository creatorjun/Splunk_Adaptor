# scripts/smoke_test.py
import json
import os
from pathlib import Path
import subprocess
import time
from datetime import datetime, timezone
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


PROJECT = Path(__file__).resolve().parent.parent


def environment():
    result = {}
    for line in (PROJECT / ".env").read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.lstrip().startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            result[key.strip()] = value.strip().strip('"').strip("'")
    return result


def command(*args):
    return subprocess.check_output(args, cwd=str(PROJECT), universal_newlines=True).strip()


def wait_until(action, predicate, timeout=20):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = action()
            if predicate(last):
                return last
        except (URLError, ConnectionError, OSError):
            pass
        time.sleep(0.1)
    raise AssertionError("Timed out waiting for the monitor")


def cpu_ticks():
    values = [int(value) for value in Path("/proc/stat").read_text().splitlines()[0].split()[1:9]]
    return sum(values), values[3] + values[4]


def main():
    env = environment()
    base = "http://127.0.0.1:" + env.get("MONITOR_PORT", "8080")
    token = env["MONITOR_API_TOKEN"]
    warn = Path(env.get("MONITOR_WARN_HOST_DIR", "./warn"))
    if not warn.is_absolute():
        warn = PROJECT / warn

    def api(path, value=None):
        data = None if value is None else json.dumps(value).encode("utf-8")
        request = Request(base + path, data=data, method="GET" if value is None else "PUT",
                          headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        with urlopen(request, timeout=8) as response:
            return json.load(response)

    def ready(value):
        snapshot = value.get("snapshot")
        return snapshot and snapshot.get("cpuPercent") is not None and not snapshot["errors"] and not value["lastError"]

    def update(settings):
        current = api("/api/status")
        return api("/api/settings", {"expectedVersion": current["settingsVersion"], "settings": settings})

    def warning_files():
        return set(warn.glob("*.json"))

    original = wait_until(lambda: api("/api/status"), ready)["settings"]
    original = json.loads(json.dumps(original))
    before_files = warning_files()
    result = {"testedAt": datetime.now(timezone.utc).isoformat(), "checks": []}
    container = command("docker", "compose", "ps", "-q", "monitor")
    started = command("docker", "inspect", container, "--format", "{{.State.StartedAt}}")

    try:
        baseline = json.loads(json.dumps(original))
        baseline["sampleIntervalSeconds"] = 1
        baseline["consecutiveSamples"] = 1
        baseline["repeatIntervalSeconds"] = 0
        baseline["diskPaths"] = ["/", "/var"]
        for resource in ["cpu", "memory", "disk"]:
            baseline[resource] = {"enabled": True, "thresholdPercent": 100, "hysteresisPercent": 0}
        previous = api("/api/status")["snapshot"]["capturedAt"]
        update(baseline)
        first = wait_until(lambda: api("/api/status"), lambda value: ready(value)
                           and value["snapshot"]["capturedAt"] != previous and len(value["snapshot"]["disks"]) == 2)
        total, idle = cpu_ticks()
        second = wait_until(lambda: api("/api/status"), lambda value: ready(value)
                            and value["snapshot"]["capturedAt"] != first["snapshot"]["capturedAt"])
        total_after, idle_after = cpu_ticks()
        host_cpu = 100.0 * ((total_after - total) - (idle_after - idle)) / (total_after - total)
        measured_cpu = second["snapshot"]["cpuPercent"]
        assert abs(host_cpu - measured_cpu) <= 8, "CPU differs from host counter delta"
        meminfo = {}
        for line in Path("/proc/meminfo").read_text().splitlines():
            key, value = line.split(":", 1)
            meminfo[key] = int(value.split()[0])
        snapshot = second["snapshot"]
        assert snapshot["memory"]["totalBytes"] == meminfo["MemTotal"] * 1024, "Host RAM total mismatch"
        assert abs(snapshot["memory"]["availableBytes"] - meminfo["MemAvailable"] * 1024) < 512 * 1024 ** 2, "Host RAM available mismatch"
        for disk in snapshot["disks"]:
            stat = os.statvfs(disk["path"])
            assert disk["totalBytes"] == stat.f_blocks * stat.f_frsize, "Host disk total mismatch"
            assert abs(disk["freeBytes"] - stat.f_bfree * stat.f_frsize) < 64 * 1024 ** 2, "Host disk free mismatch"
        result["hostComparison"] = {"cpuPercent": host_cpu, "monitorCpuPercent": measured_cpu,
                                    "memoryTotalBytes": snapshot["memory"]["totalBytes"],
                                    "disks": snapshot["disks"]}
        result["checks"].append("host CPU, RAM and filesystem comparison")

        low = json.loads(json.dumps(baseline))
        for resource in ["cpu", "memory", "disk"]:
            low[resource]["thresholdPercent"] = 0.01
        changed_at = time.monotonic()
        update(low)
        expected = {"cpu", "memory", "disk:/", "disk:/var"}
        reached = wait_until(lambda: api("/api/status"), lambda value: expected.issubset(
            {event["resource"] for event in value["recentEvents"] if event["type"] == "THRESHOLD_REACHED"
             and event["thresholdPercent"] == 0.01}))
        result["thresholdAppliedSeconds"] = round(time.monotonic() - changed_at, 3)
        assert command("docker", "inspect", container, "--format", "{{.State.StartedAt}}") == started, "Settings update restarted the process"
        generated = warning_files() - before_files
        warnings = [json.loads(path.read_text(encoding="utf-8")) for path in generated]
        assert len(warnings) == 4, "Expected a JSON file for each reached resource"
        assert {event["resource"] for event in warnings} == expected
        for event in warnings:
            assert event["usedPercent"] >= event["thresholdPercent"]
            assert event["snapshot"]["cpuPercent"] is not None
            assert event["snapshot"]["memory"] is not None
            assert len(event["snapshot"]["disks"]) == 2
            assert not event["snapshot"]["errors"]
        assert not list(warn.glob(".monitor-*.tmp")), "Temporary warning file leaked"
        result["checks"].append("live threshold change and four complete warning JSON files")

        repeat = json.loads(json.dumps(low))
        repeat["cpu"]["enabled"] = False
        repeat["disk"]["enabled"] = False
        repeat["repeatIntervalSeconds"] = 2
        update(repeat)
        wait_until(lambda: api("/api/status"), lambda value: any(
            event["type"] == "THRESHOLD_REMINDER" and event["resource"] == "memory"
            for event in value["recentEvents"]))
        repeat["repeatIntervalSeconds"] = 0
        update(repeat)
        repeat_files = warning_files()
        assert len(repeat_files - before_files) == 5, "Expected one reminder JSON"
        result["checks"].append("reminder interval and warning file creation")

        recovery = json.loads(json.dumps(baseline))
        recovery["cpu"]["enabled"] = False
        recovery["disk"]["enabled"] = False
        update(recovery)
        wait_until(lambda: api("/api/status"), lambda value: any(
            event["type"] == "RECOVERED" and event["resource"] == "memory"
            and event["thresholdPercent"] == 100 for event in value["recentEvents"]))
        assert warning_files() == repeat_files, "Recovery incorrectly created a warning file"
        result["checks"].append("recovery journal without extra warn JSON")

        status = api("/api/status")
        stale = {"expectedVersion": status["settingsVersion"] - 1, "settings": recovery}
        try:
            api("/api/settings", stale)
            raise AssertionError("Stale settings update accepted")
        except HTTPError as exception:
            assert exception.code == 409
        result["checks"].append("concurrent settings version conflict")

        update(original)
        command("docker", "compose", "restart", "monitor")
        restarted = wait_until(lambda: api("/api/status"), ready)
        assert restarted["settings"] == original, "Settings did not survive restart"
        assert warning_files() == repeat_files, "Warning files did not survive restart"
        assert api("/health")["status"] == "UP"
        result["checks"].append("restart persistence and healthy final state")
        result["warningFilesCreated"] = len(repeat_files - before_files)
        result["result"] = "PASS"
    finally:
        update(original)

    output = PROJECT / "verification" / "oracle-vm"
    output.mkdir(parents=True, exist_ok=True)
    (output / "smoke-report.json").write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
