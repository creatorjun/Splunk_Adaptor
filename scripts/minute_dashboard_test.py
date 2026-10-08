# scripts/minute_dashboard_test.py
import json
from datetime import datetime, timezone
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from smoke_test import PROJECT, command, environment, wait_until


def captured_at(value):
    whole, _, fraction = value.rstrip("Z").partition(".")
    return datetime.strptime(whole, "%Y-%m-%dT%H:%M:%S").replace(
        tzinfo=timezone.utc, microsecond=int((fraction + "000000")[:6]))


def main():
    env = environment()
    base = "http://127.0.0.1:" + env.get("MONITOR_PORT", "8080")

    def api(path):
        request = Request(base + path, headers={"Authorization": "Bearer " + env["MONITOR_API_TOKEN"]})
        with urlopen(request, timeout=8) as response:
            return json.load(response)

    def new_point(previous):
        deadline = time.monotonic() + 80
        while time.monotonic() < deadline:
            history = api("/api/history")
            if history["points"] and history["points"][-1]["capturedAt"] != previous:
                return history
            time.sleep(2)
        raise AssertionError("No new minute point within 80 seconds")

    def validate(history):
        assert history["intervalSeconds"] == 60 and history["retentionMinutes"] == 60
        assert history["lastError"] is None
        assert 0 < len(history["points"]) <= 60
        for point in history["points"]:
            assert not point["errors"]
            for resource in ["cpuPercent", "memoryPercent", "diskPercent"]:
                assert point[resource] is not None and 0 <= point[resource] <= 100
        return [(captured_at(second["capturedAt"]) - captured_at(first["capturedAt"])).total_seconds()
                for first, second in zip(history["points"], history["points"][1:])]

    result = {"testedAt": datetime.now(timezone.utc).isoformat(), "checks": []}
    try:
        with urlopen(base + "/api/history", timeout=8):
            raise AssertionError("Minute history is accessible without authentication")
    except HTTPError as exception:
        assert exception.code == 401
    result["checks"].append("minute history requires authentication")

    with urlopen(base + "/", timeout=8) as response:
        html = response.read().decode("utf-8")
    with urlopen(base + "/app.js", timeout=8) as response:
        javascript = response.read().decode("utf-8")
    assert html.count("<canvas ") == 1 and 'id="usage-chart"' in html
    assert "const POLL_INTERVAL_MS = 60000;" in javascript
    result["checks"].append("one combined chart and sixty-second browser polling")

    original_settings = api("/api/status")["settings"]
    initial = wait_until(lambda: api("/api/history"), lambda value: bool(value["points"]))
    validate(initial)
    later = new_point(initial["points"][-1]["capturedAt"])
    intervals = validate(later)
    assert 60 <= intervals[-1] < 65, "Minute collection interval differs from sixty seconds"
    result["firstIntervalSeconds"] = round(intervals[-1], 3)
    result["checks"].append("live host CPU, memory and disk collection every sixty seconds")

    container = command("docker", "compose", "ps", "-q", "monitor")
    stored = json.loads(command("docker", "exec", container, "cat", "/data/minute-history.json"))
    stored_by_time = {point["capturedAt"]: point for point in stored}
    assert all(stored_by_time.get(point["capturedAt"]) == point for point in later["points"])
    result["checks"].append("history API matches persisted JSON")

    before_restart = api("/api/history")
    command("docker", "compose", "restart", "monitor")
    restored = wait_until(lambda: api("/api/history"), lambda value: bool(value["points"]))
    restored_by_time = {point["capturedAt"]: point for point in restored["points"]}
    assert all(restored_by_time.get(point["capturedAt"]) == point for point in before_restart["points"])
    after_restart = new_point(restored["points"][-1]["capturedAt"])
    intervals = validate(after_restart)
    assert all(interval >= 60 for interval in intervals), "Restart duplicated a recent point"
    assert 60 <= intervals[-1] < 65, "Restart delayed the next minute point"
    assert api("/api/status")["settings"] == original_settings
    assert api("/health")["status"] == "UP"
    result["restartIntervalSeconds"] = round(intervals[-1], 3)
    result["pointsAfterRestart"] = len(after_restart["points"])
    result["checks"].append("restart preserves history and settings without duplicate minute points")
    result["result"] = "PASS"
    output = PROJECT / "verification" / "oracle-vm"
    output.mkdir(parents=True, exist_ok=True)
    (output / "minute-dashboard-report.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
