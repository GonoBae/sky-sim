#!/usr/bin/env python3
"""Verify SkySim runtime-clock progression from an Unreal Engine log.

The SkySim actor periodically emits lines in this form::

    SkySim clock: source=server utc=2026-08-26T07:00:00.000Z \
        local=2026-08-26T16:00:00.000 scale=60.000 sun_elevation_deg=32.500

This tool uses the Unreal log timestamp at the start of each line as wall time.
It compares adjacent samples whose time scale is unchanged, verifies that the
UTC (and local) clock advances by ``wall_delta * scale``, and applies a tighter
absolute tolerance to paused (scale=0) samples.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timedelta
import math
from pathlib import Path
import re
from typing import Sequence


WALL_TIME_RE = re.compile(
    r"\[(?P<wall>\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\]"
)
CLOCK_RE = re.compile(
    r"SkySim clock:\s+"
    r"source=(?P<source>\S+)\s+"
    r"utc=(?P<utc>\S+)\s+"
    r"local=(?P<local>\S+)\s+"
    r"scale=(?P<scale>[-+0-9.eE]+)\s+"
    r"sun_elevation_deg=(?P<sun>[-+0-9.eE]+)"
)


@dataclass(frozen=True)
class ClockSample:
    line_number: int
    wall_time: datetime
    source: str
    utc_time: datetime
    local_time: datetime
    scale: float
    sun_elevation_degrees: float


@dataclass(frozen=True)
class PairResult:
    previous: ClockSample
    current: ClockSample
    wall_delta_seconds: float
    simulated_delta_seconds: float
    expected_delta_seconds: float
    allowed_error_seconds: float
    error_seconds: float
    paused: bool
    failure: str | None = None


def _parse_iso8601(value: str) -> datetime:
    # datetime.fromisoformat accepts offsets but recognizes the widely used Z
    # suffix only on newer Python versions. Replacing it keeps the tool usable
    # with the Python bundled alongside older Unreal installations.
    if value.endswith(("Z", "z")):
        value = value[:-1] + "+00:00"
    return datetime.fromisoformat(value)


def parse_clock_samples(lines: Sequence[str]) -> tuple[list[ClockSample], list[str]]:
    samples: list[ClockSample] = []
    errors: list[str] = []

    for line_number, line in enumerate(lines, 1):
        if "SkySim clock:" not in line:
            continue

        wall_match = WALL_TIME_RE.search(line)
        clock_match = CLOCK_RE.search(line)
        if wall_match is None or clock_match is None:
            errors.append(f"line {line_number}: malformed SkySim clock diagnostic")
            continue

        try:
            wall_time = datetime.strptime(wall_match.group("wall"), "%Y.%m.%d-%H.%M.%S:%f")
            utc_time = _parse_iso8601(clock_match.group("utc"))
            local_time = _parse_iso8601(clock_match.group("local"))
            scale = float(clock_match.group("scale"))
            sun_elevation = float(clock_match.group("sun"))
        except ValueError as exc:
            errors.append(f"line {line_number}: invalid diagnostic value ({exc})")
            continue

        if not math.isfinite(scale) or not math.isfinite(sun_elevation):
            errors.append(f"line {line_number}: scale and sun elevation must be finite")
            continue

        samples.append(
            ClockSample(
                line_number=line_number,
                wall_time=wall_time,
                source=clock_match.group("source"),
                utc_time=utc_time,
                local_time=local_time,
                scale=scale,
                sun_elevation_degrees=sun_elevation,
            )
        )

    return samples, errors


def evaluate_pairs(
    samples: Sequence[ClockSample],
    *,
    relative_tolerance: float,
    absolute_tolerance_seconds: float,
    pause_tolerance_seconds: float,
    local_tolerance_seconds: float,
    minimum_wall_seconds: float,
) -> tuple[list[PairResult], int]:
    results: list[PairResult] = []
    skipped = 0

    for previous, current in zip(samples, samples[1:]):
        wall_delta = (current.wall_time - previous.wall_time).total_seconds()
        scale_tolerance = max(1e-6, abs(previous.scale) * 1e-6)

        # A backwards wall clock indicates that two editor sessions were
        # appended to one file. A scale transition has no exact transition
        # timestamp, so neither interval can be validated fairly.
        if wall_delta < minimum_wall_seconds or not math.isclose(
            previous.scale, current.scale, rel_tol=0.0, abs_tol=scale_tolerance
        ):
            skipped += 1
            continue

        simulated_delta = (current.utc_time - previous.utc_time).total_seconds()
        local_delta = (current.local_time - previous.local_time).total_seconds()
        expected_delta = wall_delta * previous.scale
        paused = math.isclose(previous.scale, 0.0, rel_tol=0.0, abs_tol=1e-6)
        allowed_error = (
            pause_tolerance_seconds
            if paused
            else absolute_tolerance_seconds + abs(expected_delta) * relative_tolerance
        )
        error = abs(simulated_delta - expected_delta)

        failures: list[str] = []
        if error > allowed_error:
            failures.append(
                f"UTC error {error:.3f}s exceeds allowed {allowed_error:.3f}s"
            )
        local_error = abs(local_delta - simulated_delta)
        if local_error > local_tolerance_seconds:
            failures.append(
                f"local/UTC delta mismatch {local_error:.3f}s exceeds "
                f"allowed {local_tolerance_seconds:.3f}s"
            )

        results.append(
            PairResult(
                previous=previous,
                current=current,
                wall_delta_seconds=wall_delta,
                simulated_delta_seconds=simulated_delta,
                expected_delta_seconds=expected_delta,
                allowed_error_seconds=allowed_error,
                error_seconds=error,
                paused=paused,
                failure="; ".join(failures) if failures else None,
            )
        )

    return results, skipped


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, nargs="?", help="Unreal Engine .log file")
    parser.add_argument(
        "--relative-tolerance",
        type=float,
        default=0.10,
        help="fractional tolerance for running-clock UTC deltas (default: 0.10)",
    )
    parser.add_argument(
        "--absolute-tolerance-seconds",
        type=float,
        default=0.5,
        help="base running-clock UTC delta tolerance (default: 0.5)",
    )
    parser.add_argument(
        "--pause-tolerance-seconds",
        type=float,
        default=0.05,
        help="maximum UTC movement between scale=0 samples (default: 0.05)",
    )
    parser.add_argument(
        "--local-tolerance-seconds",
        type=float,
        default=0.05,
        help="maximum difference between local and UTC deltas (default: 0.05)",
    )
    parser.add_argument(
        "--minimum-wall-seconds",
        type=float,
        default=0.05,
        help="ignore duplicate or cross-session samples closer than this (default: 0.05)",
    )
    parser.add_argument(
        "--require-running",
        action="store_true",
        help="fail unless at least one nonzero-scale interval is validated",
    )
    parser.add_argument(
        "--require-paused",
        action="store_true",
        help="fail unless at least one scale=0 interval is validated",
    )
    parser.add_argument(
        "--require-sun-change",
        type=float,
        metavar="DEGREES",
        help="fail unless running samples span at least this sun-elevation change",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="validate the parser and pass/fail rules using built-in sample logs",
    )
    return parser


def _validate_nonnegative(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    for name in (
        "relative_tolerance",
        "absolute_tolerance_seconds",
        "pause_tolerance_seconds",
        "local_tolerance_seconds",
        "minimum_wall_seconds",
        "require_sun_change",
    ):
        value = getattr(args, name)
        if value is not None and value < 0.0:
            parser.error(f"--{name.replace('_', '-')} must be nonnegative")


def _run_self_test() -> int:
    sample_lines = [
        "[2026.09.04-10.00.00:000][  0]LogSkySim: Display: "
        "SkySim clock: source=server utc=2026-09-04T01:00:00.000Z "
        "local=2026-09-04T10:00:00.000 scale=60.000 sun_elevation_deg=10.000\n",
        "[2026.09.04-10.00.02:000][120]LogSkySim: Display: "
        "SkySim clock: source=server utc=2026-09-04T01:02:00.000Z "
        "local=2026-09-04T10:02:00.000 scale=60.000 sun_elevation_deg=10.500\n",
        "[2026.09.04-10.00.03:000][180]LogSkySim: Display: "
        "SkySim clock: source=server utc=2026-09-04T01:02:00.000Z "
        "local=2026-09-04T10:02:00.000 scale=0.000 sun_elevation_deg=10.500\n",
        "[2026.09.04-10.00.05:000][300]LogSkySim: Display: "
        "SkySim clock: source=server utc=2026-09-04T01:02:00.000Z "
        "local=2026-09-04T10:02:00.000 scale=0.000 sun_elevation_deg=10.500\n",
    ]
    samples, parse_errors = parse_clock_samples(sample_lines)
    results, _ = evaluate_pairs(
        samples,
        relative_tolerance=0.01,
        absolute_tolerance_seconds=0.01,
        pause_tolerance_seconds=0.01,
        local_tolerance_seconds=0.01,
        minimum_wall_seconds=0.05,
    )
    if parse_errors or len(results) != 2 or any(result.failure for result in results):
        print(f"SELF-TEST FAIL: parse_errors={parse_errors!r} results={results!r}")
        return 1

    broken_lines = list(sample_lines)
    broken_lines[1] = broken_lines[1].replace("01:02:00.000Z", "01:00:20.000Z")
    broken_samples, _ = parse_clock_samples(broken_lines)
    broken_results, _ = evaluate_pairs(
        broken_samples,
        relative_tolerance=0.01,
        absolute_tolerance_seconds=0.01,
        pause_tolerance_seconds=0.01,
        local_tolerance_seconds=0.01,
        minimum_wall_seconds=0.05,
    )
    if not broken_results or broken_results[0].failure is None:
        print("SELF-TEST FAIL: a deliberately incorrect running sample passed")
        return 1

    print("SELF-TEST PASS: parser, running clock, pause, and failure detection")
    return 0


def run(args: argparse.Namespace) -> int:
    if args.self_test:
        return _run_self_test()
    if args.log is None:
        print("ERROR: a log path is required unless --self-test is used")
        return 2
    if not args.log.is_file():
        print(f"ERROR: log file does not exist: {args.log}")
        return 2

    lines = args.log.read_text(encoding="utf-8", errors="replace").splitlines()
    samples, parse_errors = parse_clock_samples(lines)
    for error in parse_errors:
        print(f"FAIL {error}")

    if len(samples) < 2:
        print(
            f"FAIL: found {len(samples)} valid SkySim clock sample(s); "
            "at least two are required"
        )
        return 2

    results, skipped = evaluate_pairs(
        samples,
        relative_tolerance=args.relative_tolerance,
        absolute_tolerance_seconds=args.absolute_tolerance_seconds,
        pause_tolerance_seconds=args.pause_tolerance_seconds,
        local_tolerance_seconds=args.local_tolerance_seconds,
        minimum_wall_seconds=args.minimum_wall_seconds,
    )
    running_results = [result for result in results if not result.paused]
    paused_results = [result for result in results if result.paused]

    for result in results:
        status = "FAIL" if result.failure else "PASS"
        source = (
            result.previous.source
            if result.previous.source == result.current.source
            else f"{result.previous.source}->{result.current.source}"
        )
        print(
            f"{status} lines {result.previous.line_number}->{result.current.line_number} "
            f"source={source} scale={result.previous.scale:g} "
            f"wall={result.wall_delta_seconds:.3f}s "
            f"simulated={result.simulated_delta_seconds:.3f}s "
            f"expected={result.expected_delta_seconds:.3f}s "
            f"error={result.error_seconds:.3f}s/{result.allowed_error_seconds:.3f}s"
        )
        if result.failure:
            print(f"  {result.failure}")

    requirement_failures: list[str] = []
    if not results:
        requirement_failures.append("no comparable same-scale sample pairs")
    if args.require_running and not running_results:
        requirement_failures.append("no running (nonzero-scale) pair was validated")
    if args.require_paused and not paused_results:
        requirement_failures.append("no paused (scale=0) pair was validated")
    if args.require_sun_change is not None:
        running_samples = [sample for sample in samples if abs(sample.scale) > 1e-6]
        sun_span = (
            max(sample.sun_elevation_degrees for sample in running_samples)
            - min(sample.sun_elevation_degrees for sample in running_samples)
            if running_samples
            else 0.0
        )
        if sun_span < args.require_sun_change:
            requirement_failures.append(
                f"running sun-elevation span {sun_span:.3f}deg is below "
                f"required {args.require_sun_change:.3f}deg"
            )

    print(
        f"summary: samples={len(samples)} pairs={len(results)} "
        f"running={len(running_results)} paused={len(paused_results)} "
        f"skipped={skipped} malformed={len(parse_errors)}"
    )
    for failure in requirement_failures:
        print(f"FAIL: {failure}")

    failed_pairs = sum(result.failure is not None for result in results)
    if parse_errors or failed_pairs or requirement_failures:
        print("SkySim time progression verification FAILED")
        return 1

    print("SkySim time progression verification PASSED")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    _validate_nonnegative(parser, args)
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
