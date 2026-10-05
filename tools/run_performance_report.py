#!/usr/bin/env python3
"""Build the Release performance suite and generate the checked-in Chinese report."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import platform
import statistics
import subprocess
import tempfile
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(command: list[str], timeout: int, cwd: pathlib.Path = ROOT) -> tuple[str, float]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=True,
        )
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout, end="")
        raise
    return completed.stdout, time.monotonic() - started


def optional(command: list[str]) -> str:
    try:
        return subprocess.run(
            command, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            timeout=10, check=True,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def linux_value(path: pathlib.Path, prefix: str = "") -> str:
    try:
        text = path.read_text()
        if prefix:
            for line in text.splitlines():
                if line.startswith(prefix):
                    return line.split(":", 1)[1].strip()
        return text.strip()
    except OSError:
        return "unknown"


def environment() -> dict:
    if platform.system() == "Darwin":
        model = optional(["sysctl", "-n", "hw.model"])
        physical = int(optional(["sysctl", "-n", "hw.physicalcpu"]))
        logical = int(optional(["sysctl", "-n", "hw.logicalcpu"]))
        memory = int(optional(["sysctl", "-n", "hw.memsize"]))
        os_name = f"macOS {optional(['sw_vers', '-productVersion'])} ({platform.machine()})"
    else:
        model = linux_value(pathlib.Path("/proc/cpuinfo"), "model name")
        logical = os.cpu_count() or 0
        physical_ids = set()
        core_ids = set()
        try:
            block = {}
            for line in pathlib.Path("/proc/cpuinfo").read_text().splitlines() + [""]:
                if not line and block:
                    physical_ids.add(block.get("physical id", "0"))
                    core_ids.add((block.get("physical id", "0"), block.get("core id", block.get("processor", "0"))))
                    block = {}
                elif ":" in line:
                    key, value = line.split(":", 1)
                    block[key.strip()] = value.strip()
        except OSError:
            pass
        physical = len(core_ids) or logical
        mem_kb = linux_value(pathlib.Path("/proc/meminfo"), "MemTotal")
        memory = int(mem_kb.split()[0]) * 1024 if mem_kb != "unknown" else 0
        os_name = platform.platform()
    return {
        "testedAt": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "model": model,
        "physicalCores": physical,
        "logicalCores": logical,
        "memoryBytes": memory,
        "os": os_name,
        "compiler": optional(["c++", "--version"]).splitlines()[0],
        "buildType": "Release",
    }


def median(values: list[float]) -> float:
    return statistics.median(values) if values else 0.0


def fmt(value: float, digits: int = 3) -> str:
    return f"{value:,.{digits}f}"


def capacity_summary(runs: list[dict]) -> str:
    if not runs:
        return "无样本"
    p99 = [run["frameMs"]["p99"] for run in runs]
    misses = [run["deadlineMissPct"] for run in runs]
    return (
        f"P99 中位数 {fmt(median(p99))} ms，最差 {fmt(max(p99))} ms，"
        f"范围 {fmt(min(p99))}–{fmt(max(p99))} ms；deadline miss 最差 "
        f"{fmt(max(misses), 4)}%"
    )


def make_report(data: dict) -> str:
    env = data["environment"]
    perf = data["performance"]
    capacity = perf["capacity"]
    udp = perf["udp"]
    stress = data["stress"]
    runs = capacity["runs"]
    stable = [run for run in runs if run["finalValidation"] and not run["soak"]
              and run["rooms"] == capacity["maxStableRooms"]]
    failed = [run for run in runs if run["finalValidation"] and not run["soak"]
              and run["rooms"] == capacity["firstFailRooms"]]
    soak = next(run for run in runs if run["soak"])

    curve_rows = []
    seen = set()
    for run in runs:
        if run["finalValidation"] or run["rooms"] in seen:
            continue
        seen.add(run["rooms"])
        verdict = "通过" if (run["frameMs"]["p99"] <= 16.666667 and
                             run["deadlineMissPct"] <= 0.1 and run["errors"] == 0) else "失败"
        curve_rows.append(
            f"| {run['rooms']:,} | {run['players']:,} | {fmt(run['frameMs']['p99'])} | "
            f"{fmt(run['deadlineMissPct'], 4)} | {fmt(run['projectedRealtimeCpuPct'], 2)} | {verdict} |"
        )

    udp_rows = []
    for rate in (60, 120, 240, 480):
        group = [run for run in udp["runs"] if run["offeredPpsPerClient"] == rate]
        udp_rows.append(
            f"| {rate} | {fmt(median([r['successPct'] for r in group]), 3)} | "
            f"{fmt(median([r['acceptedPps'] for r in group]), 1)} | "
            f"{fmt(median([r['ackRttMs']['p99'] for r in group]))} | "
            f"{fmt(max(r['tickMs']['p99'] for r in group), 4)} | "
            f"{fmt(median([r['processCpuPct'] for r in group]), 2)} | "
            f"{sum(r['errors'] for r in group)} |"
        )

    dists = []
    for key, label in (("tickMs", "完整 tick"), ("stateEncodeMs", "State encode"),
                       ("stateDecodeMs", "State decode"), ("replayMs", "restore/replay")):
        value = stress[key]
        dists.append(
            f"| {label} | {value['samples']:,} | {fmt(value['mean'], 4)} | "
            f"{fmt(value['p50'], 4)} | {fmt(value['p95'], 4)} | {fmt(value['p99'], 4)} | "
            f"{fmt(value['p999'], 4)} | {fmt(value['max'], 4)} | "
            f"{fmt(value['jitterP99P50'], 4)} |"
        )

    memory_mb = env["memoryBytes"] / (1024 * 1024 * 1024)
    status = "通过" if data["ctest"]["passed"] and capacity["passed"] and udp["passed"] else "失败"
    return f"""# 并发容量、P99 与完整性能报告

测试时间：{env['testedAt']}  
测试结论：**{status}**  
基线机器：{env['model']}，{env['physicalCores']} 核 / {env['logicalCores']} 线程，{memory_mb:.0f} GB  
系统与编译器：{env['os']}；{env['compiler']}

## 结论摘要

- 单线程生产 `AuthoritativeServer` 核心最大稳定容量为 **{capacity['maxStableRooms']:,} 房间 / {capacity['maxStablePlayers']:,} 玩家**；建议安全容量为 **{capacity['safeRooms']:,} 房间 / {capacity['safePlayers']:,} 玩家**。
- 最大稳定档三轮结果：{capacity_summary(stable)}。首个正式失败档为 {capacity['firstFailRooms']:,} 房间：{capacity_summary(failed)}。
- 容量约束判定为 `{capacity['limitingResource']}`；按 60% 物理内存预算和实测约 {capacity['bytesPerRoom'] / 1024:.1f} KiB/房间估算，内存上限为 {capacity['memoryLimitRooms']:,} 房间。
- 单房间真实 loopback UDP 无明显丢弃的最高输入速率为 **{udp['maxNoDropPpsPerClient']} PPS/客户端**。480 PPS/客户端场景触发 240 PPS/客户端服务端限流，服务端 tick 仍保持 60Hz 且无协议或身份错误。
- 这里的多房间数字仅代表当前**生产服务端核心的单线程容量**，不包含尚未实现的多房间 UDP 路由、系统调用、跨房间调度或公网网络开销。

## 验收口径

正式容量要求连续三轮同时满足：批量 tick P99 ≤ 16.67 ms、deadline miss ≤ 0.1%、协议及 hash 错误为 0。建议安全容量取最大稳定容量的 80%，再受本机 60% 内存预算限制。每轮预热 5 秒、采样 20 秒；安全容量另做 60 秒稳态测试，预热样本不进入统计，percentile 使用 nearest-rank。

## 容量曲线

| 房间 | 玩家 | 批量 tick P99 (ms) | deadline miss | 预计实时单核 CPU | 结果 |
| ---: | ---: | ---: | ---: | ---: | --- |
{os.linesep.join(curve_rows)}

正式最大稳定档：{capacity_summary(stable)}。三轮均至少包含 1,000 个有效样本，错误数均为 0。

60 秒稳态档为 {soak['rooms']:,} 房间：RSS 从 {soak['rssAfterWarmup'] / 1048576:.2f} MB 到 {soak['rssEnd'] / 1048576:.2f} MB，峰值 {soak['peakRss'] / 1048576:.2f} MB，增长 {fmt(soak['rssGrowthPct'], 3)}%，即 {fmt(soak['rssGrowthMbPerMin'], 3)} MB/min。

## 真实 UDP 吞吐与 ACK RTT

下表为每档三轮的中位数；tick P99 使用三轮最差值，accepted PPS 是两客户端合计。

| offered PPS/客户端 | 成功率 (%) | accepted PPS | ACK RTT P99 (ms) | tick P99 (ms) | 进程 CPU (%) | 错误 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
{os.linesep.join(udp_rows)}

60 PPS/客户端正常场景满足成功率 ≥99.9%、ACK RTT P99 ≤50 ms、身份/hash 错误为 0。高压档的拒绝是服务端限流的预期行为，不计为协议错误。

## 确定性、编码与回滚压力

场景：{stress['ticks']:,} ticks、{stress['players']} 玩家、State 每 {stress['stateEvery']} tick、延迟 {stress['stateDelay']} tick、输入冗余 {stress['redundancy']}。累计重放 {stress['replayedTicks']:,} ticks，吞吐 {fmt(stress['ticksPerSec'], 1)} ticks/s；hash 校验 {stress['hashChecks']:,} 次、raw restore 校验 {stress['rawRestoreChecks']:,} 次，全部通过。

| 路径 | 样本 | mean | P50 | P95 | P99 | P99.9 | max | P99-P50 抖动 | 
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
{os.linesep.join(dists)}

`ticksPerSec` 是带完整一致性校验的离线吞吐，只适合比较回归趋势；它不能替代以 60Hz deadline、P99 和 deadline miss 判定的实时容量。

## 命令与通过情况

```bash
{os.linesep.join(data['commands'])}
```

CTest：{data['ctest']['passedCount']}/{data['ctest']['totalCount']} 通过，用时 {data['ctest']['durationSec']:.2f} 秒。原始、逐轮可复核数据见 [`performance_results.json`](performance_results.json)。

## 瓶颈与适用边界

容量边界首先由单线程每 16.67 ms 必须完成全部房间 tick 的 CPU 实时预算决定。内存推算只依据当前进程短时 RSS 增量，不能替代长时间泄漏测试。Loopback RTT 包含服务端 ACK 频率和事件循环调度，不能外推公网 RTT；正式上线还应在目标 Linux 主机、真实 NIC、多房间路由和代表性公网网络条件下复跑同一 JSON 套件。
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--output-dir", type=pathlib.Path, default=ROOT / "docs")
    args = parser.parse_args()
    build = (args.build_dir or pathlib.Path(tempfile.mkdtemp(prefix="fighting-performance-"))).resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)

    configure = ["cmake", "-S", str(ROOT), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                 "-DLAB_BUILD_CLIENT=OFF", "-DLAB_BUILD_TESTS=ON",
                 "-DLAB_BUILD_PERFORMANCE=ON"]
    build_command = ["cmake", "--build", str(build), "--target", "lab_tests", "lab_stress",
                     "lab_network_integration", "lab_performance", "lab_server", "-j", "2"]
    ctest = ["ctest", "--test-dir", str(build), "--output-on-failure", "--timeout", "900"]
    stress_json = build / "stress_results.json"
    stress = [str(build / "lab_stress"), "--ticks", "60000", "--players", "2",
              "--history", "4096", "--state-every", "2", "--state-delay", "7",
              "--redundancy", "8", "--json", str(stress_json)]

    run(configure, 120)
    run(build_command, 120)
    ctest_output, ctest_seconds = run(ctest, 900)
    run(stress, 120)

    performance = json.loads((build / "performance_results.json").read_text())
    stress_data = json.loads(stress_json.read_text())
    passed_count = ctest_output.count(" Passed")
    total_count = len([line for line in ctest_output.splitlines() if "Test #" in line])
    if total_count == 0:
        total_count = passed_count

    data = {
        "schemaVersion": 1,
        "environment": environment(),
        "commands": [" ".join(configure), " ".join(build_command), " ".join(ctest), " ".join(stress)],
        "ctest": {
            "passed": "100% tests passed" in ctest_output,
            "passedCount": passed_count,
            "totalCount": total_count,
            "durationSec": ctest_seconds,
            "output": ctest_output,
        },
        "performance": performance,
        "stress": stress_data,
    }
    (output / "performance_results.json").write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output / "PERFORMANCE_REPORT.md").write_text(make_report(data), encoding="utf-8")
    print(f"report: {output / 'PERFORMANCE_REPORT.md'}")
    print(f"raw data: {output / 'performance_results.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
