#!/usr/bin/env python3
"""Time all six untouched EPQ examples and verify their complete transcripts."""

import argparse
import hashlib
import heapq
import json
import os
from pathlib import Path
import platform
import random
import re
import statistics
import subprocess
import tempfile
import time


def digest(data):
    return hashlib.sha256(data).hexdigest()


def transcript(prompt_count, answer):
    return ("<< " * prompt_count + ">> " + str(answer) + "\n").encode()


def graph_case():
    # The final 32 vertices are unreachable. The rest form a connected chain
    # with extra positive-cost edges; updates exercise the example's heap.
    vertices = 2048
    connected = vertices - 32
    rng = random.Random(73)
    edges = [(v - 1, v, rng.randint(1, 1000)) for v in range(1, connected)]
    while len(edges) < 8192:
        u, v = rng.randrange(connected), rng.randrange(connected)
        if u != v:
            edges.append((u, v, rng.randint(1, 1000)))
    adj = [[] for _ in range(vertices)]
    for u, v, weight in edges:
        adj[u].append((v, weight))
        adj[v].append((u, weight))
    distances = [-1] * vertices
    distances[0] = 0
    heap = [(0, 0)]
    while heap:
        cost, u = heapq.heappop(heap)
        if distances[u] != cost:
            continue
        for v, weight in adj[u]:
            new_cost = cost + weight
            if distances[v] < 0 or new_cost < distances[v]:
                distances[v] = new_cost
                heapq.heappush(heap, (new_cost, v))
    records = [f"{vertices} {len(edges)}", *(f"{u} {v} {w}" for u, v, w in edges)]
    return ("\n".join(records) + "\n").encode(), transcript(2 + 3 * len(edges), distances)


def examples(upstream):
    value = 75
    values = []
    for _ in range(5000):
        value = (value * 75 + 74) % 65537
        values.append(value)
    sorted_output = transcript(0, sorted(values))
    cases = {
        "fibonacci": (b"45\n", transcript(1, 1134903170)),
        "eratosthenes": (b"1000000\n", transcript(1, 78498)),
        "dijkstra": graph_case(),
        "bubblesort": (b"", sorted_output),
        "heapsort": (b"", sorted_output),
        "mergesort": (b"", sorted_output),
    }
    result = {}
    for source in sorted((upstream / "examples").rglob("*.txt")):
        name = source.stem
        if name in result or name not in cases:
            raise ValueError(f"unexpected example {source}")
        data, expected = cases[name]
        result[name] = (source, data, expected)
    if result.keys() != cases.keys():
        raise ValueError(f"missing examples: {cases.keys() - result.keys()}")
    return result


def execute(command, input_file, output, env, timeout=None):
    input_file.seek(0)
    started = time.perf_counter_ns()
    result = subprocess.run(command, stdin=input_file, stdout=output, stderr=subprocess.PIPE,
                            env=env, timeout=timeout)
    elapsed = time.perf_counter_ns() - started
    if result.returncode:
        raise RuntimeError(f"{command} exited {result.returncode}: {result.stderr.decode(errors='replace')}")
    return elapsed, result


def execution_time(stderr, unit):
    match = re.fullmatch(rb"Execution time: (\d+) " + unit + rb"\s*", stderr)
    if not match:
        raise ValueError(f"unexpected timing output: {stderr!r}")
    return int(match.group(1)) * (1000 if unit == b"microseconds" else 1)


def check_transcript(result, expected, variant):
    actual = result.stdout
    if actual != expected:
        position = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                        min(len(actual), len(expected)))
        raise ValueError(f"{variant} stdout differs at byte {position}. "
                         f"Expected {expected[position:position+90]!r}, got {actual[position:position+90]!r}. "
                         f"Lengths {len(expected)} and {len(actual)}.")
    if result.stderr:
        raise ValueError(f"{variant} wrote unexpected stderr: {result.stderr[:400]!r}")


def measure(name, case, args, temp, env):
    source, data, expected = case
    native = temp / name
    previous_native = temp / (name + "-previous")
    input_path = temp / (name + ".input")
    input_path.write_bytes(data)
    with input_path.open("rb") as stdin, open(os.devnull, "wb") as sink:
        builds = []
        for _ in range(args.build_trials):
            dt, _ = execute([args.compiler, "build", source, "-o", native], stdin, sink, env, timeout=120)
            builds.append(dt)
        originals = {"vm": args.original}
        if args.alternate_original:
            originals["alt_vm"] = args.alternate_original
        commands = {key: [binary, source] for key, binary in originals.items()}
        commands.update({"native": [native], "jit": [args.compiler, source]})
        if args.previous_compiler:
            execute([args.previous_compiler, "build", source, "-o", previous_native],
                    stdin, sink, env, timeout=120)
            commands.update({"previous_native": [previous_native],
                             "previous_jit": [args.previous_compiler, source]})
        for key, command in commands.items():
            _, result = execute(command, stdin, subprocess.PIPE, env, timeout=120)
            check_transcript(result, expected, f"{name}/{key}")
        samples = {key + "_wall": [] for key in commands}
        samples.update({key + "_execution": [] for key in commands})
        timed_env = dict(env, EPQ_TIMING="1")
        for trial in range(-args.warmups, args.trials):
            # Rotate the first engine so a variant doesn't always start a round.
            keys = list(commands)
            offset = trial % len(keys)
            order = keys[offset:] + keys[:offset]
            for key in order:
                elapsed, run = execute(commands[key], stdin, sink, env)
                if run.stderr:
                    raise ValueError(f"unexpected stderr: {run.stderr!r}")
                if trial >= 0:
                    samples[key + "_wall"].append(elapsed)
            for key in reversed(order):
                if key in originals:
                    _, run = execute([originals[key], "-t", source], stdin, sink, env)
                    elapsed = execution_time(run.stderr, b"microseconds")
                else:
                    _, run = execute(commands[key], stdin, sink, timed_env)
                    elapsed = execution_time(run.stderr, b"nanoseconds")
                if trial >= 0:
                    samples[key + "_execution"].append(elapsed)
    samples["build"] = builds
    record = {"source": str(source.relative_to(args.upstream)), "source_sha256": digest(source.read_bytes()),
              "native_sha256": digest(native.read_bytes()),
              "input_sha256": digest(data), "stdout_sha256": digest(expected), "stdout_bytes": len(expected),
              "samples_ns": samples}
    if args.previous_compiler:
        record["previous_native_sha256"] = digest(previous_native.read_bytes())
    return record


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("upstream", type=Path, help="read-only checkout of BoomydayCoder/EPQ")
    p.add_argument("--compiler", type=Path, required=True, help="new EPQ compiler")
    p.add_argument("--previous-compiler", type=Path,
                   help="interleave the previous CodexEPQ compiler and its standalone binaries")
    p.add_argument("--original", type=Path, required=True, help="reference VM built by build_original.py")
    p.add_argument("--original-label", default="Reference VM", help="toolchain label recorded in JSON")
    p.add_argument("--alternate-original", type=Path, help="interleave a second reference VM build")
    p.add_argument("--alternate-label", default="Alternate reference VM")
    p.add_argument("--trials", type=int, default=11)
    p.add_argument("--warmups", type=int, default=2)
    p.add_argument("--build-trials", type=int, default=3)
    p.add_argument("--only", nargs="+", help="select examples by basename")
    p.add_argument("--json", type=Path, help="write every measured sample, in nanoseconds")
    args = p.parse_args()
    if args.trials < 1 or args.build_trials < 1 or args.warmups < 0:
        p.error("trial counts must be positive and warmups nonnegative")
    args.original = args.original.resolve()
    if args.alternate_original:
        args.alternate_original = args.alternate_original.resolve()
    args.compiler = args.compiler.resolve()
    if args.previous_compiler:
        args.previous_compiler = args.previous_compiler.resolve()
    args.upstream = args.upstream.resolve()
    cases = examples(args.upstream)
    if args.only and set(args.only) - cases.keys():
        p.error("unknown example")
    ref = subprocess.check_output(["git", "-C", args.upstream, "rev-parse", "HEAD"], text=True).strip()
    report = {"upstream_commit": ref, "macos": platform.mac_ver()[0], "machine": platform.machine(),
              "compiler_sha256": digest(args.compiler.read_bytes()), "original_sha256": digest(args.original.read_bytes()),
              "original_label": args.original_label,
              "warmups": args.warmups, "trials": args.trials, "build_trials": args.build_trials, "examples": {}}
    if args.alternate_original:
        report.update(alternate_label=args.alternate_label, alternate_sha256=digest(args.alternate_original.read_bytes()))
    if args.previous_compiler:
        report["previous_compiler_sha256"] = digest(args.previous_compiler.read_bytes())
    print("All times are median milliseconds; each speedup uses the primary VM.", flush=True)
    print("Example       VM exec    Native exec  Speedup   VM wall  Native wall  Speedup   JIT wall   Speedup   Build", flush=True)
    with tempfile.TemporaryDirectory(prefix="epq-bench-") as tmp:
        env = dict(os.environ, EPQ_CACHE_DIR=str(Path(tmp) / "cache"))
        env.pop("EPQ_TIMING", None)
        for name, case in cases.items():
            if args.only and name not in args.only:
                continue
            record = measure(name, case, args, Path(tmp), env)
            report["examples"][name] = record
            ms = {key: statistics.median(values) / 1e6 for key, values in record["samples_ns"].items()}
            print(f"{name:12} {ms['vm_execution']:8.3f} {ms['native_execution']:12.3f} "
                  f"{ms['vm_execution']/ms['native_execution']:7.2f}x "
                  f"{ms['vm_wall']:8.3f} {ms['native_wall']:11.3f} {ms['vm_wall']/ms['native_wall']:7.2f}x "
                  f"{ms['jit_wall']:10.3f} {ms['vm_wall']/ms['jit_wall']:7.2f}x {ms['build']:8.1f}", flush=True)
            if args.alternate_original:
                print(f"  {args.alternate_label}: {ms['alt_vm_execution']:.3f} ms execution, "
                      f"{ms['alt_vm_wall']:.3f} ms wall", flush=True)
            if args.previous_compiler:
                print(f"  Previous / current JIT execution: {ms['previous_jit_execution']:.3f} / "
                      f"{ms['jit_execution']:.3f} ms; JIT wall: {ms['previous_jit_wall']:.3f} / "
                      f"{ms['jit_wall']:.3f} ms", flush=True)
    if digest(args.compiler.read_bytes()) != report["compiler_sha256"]:
        raise RuntimeError("EPQ compiler changed during the benchmark; rerun with a stable build")
    if args.previous_compiler and digest(args.previous_compiler.read_bytes()) != report["previous_compiler_sha256"]:
        raise RuntimeError("Previous compiler changed during measurement")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
