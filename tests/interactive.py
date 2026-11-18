#!/usr/bin/env python3
"""Check prompt delivery, blocking input, and output ordering through real pipes."""

import argparse
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time


PROGRAM = """
n := <<;
sum := 0;
(i := 0; i < |n|; i := i + 1;) @ sum := sum + <<;
(n < 0) ? {
    path := [1, 0];
    pos := 0;
    (pos >= 0) @ pos := path[pos];
}
(n = 128) ? => 0;
>> sum;
>> <<;
"""
PROMPT = b"<< "


def expect(pipe, expected):
    remaining = len(expected)
    chunks = []
    deadline = time.monotonic() + 5
    while remaining:
        ready, _, _ = select.select([pipe], [], [], max(0, deadline - time.monotonic()))
        if not ready:
            raise AssertionError(f"timeout waiting for {remaining} output bytes")
        chunk = os.read(pipe.fileno(), remaining)
        if not chunk:
            raise AssertionError(f"EOF with {remaining} output bytes outstanding")
        chunks.append(chunk)
        remaining -= len(chunk)
    assert b"".join(chunks) == expected


def handshake(command):
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0) as p:
        try:
            expect(p.stdout, PROMPT)
            p.stdin.write(b"65\n")
            expect(p.stdout, PROMPT)
            p.stdin.write(b"1 " * 64 + b"-2\n")
            expect(p.stdout, PROMPT * 64 + b">> 62\n" + PROMPT)
            p.stdin.write(b"+")
            p.stdin.write(b"7")
            p.stdin.close()
            expect(p.stdout, b">> 7\n")
            assert p.wait(timeout=5) == 0 and p.stdout.read() == b""
        finally:
            p.kill()


def observe_during_computation(command):
    # The final read goes straight into ongoing computation with no print, read,
    # or exit that could otherwise trigger a flush. Leave stdin open throughout.
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0) as p:
        try:
            expect(p.stdout, PROMPT)
            p.stdin.write(b"-64 " + b"1 " * 64)
            expect(p.stdout, PROMPT * 64)
            assert p.poll() is None
        finally:
            p.kill()


def slow_consumer(command, data_path):
    count = 100000
    data_path.write_bytes(f"{count} ".encode() + b"1 " * count + b"-9\n")
    with data_path.open("rb") as source:
        with subprocess.Popen(command, stdin=source, stdout=subprocess.PIPE) as p:
            try:
                expect(p.stdout, PROMPT)
                # The remaining output exceeds the pipe capacity. Let the writer
                # encounter backpressure and then drain the entire transcript.
                time.sleep(0.05)
                out, _ = p.communicate(timeout=10)
                assert p.returncode == 0
                assert out == PROMPT * count + b">> 100000\n<< >> -9\n"
            finally:
                p.kill()


def run(command, data_path):
    handshake(command)
    observe_during_computation(command)
    slow_consumer(command, data_path)
    early = subprocess.run(command, input=b"128 " + b"1 " * 128,
                           capture_output=True, check=True, timeout=5)
    assert early.stdout == PROMPT * 129
    bad = subprocess.run(command, input=b"100 " + b"1 " * 99 + b"x",
                         capture_output=True, timeout=5)
    assert bad.returncode > 0 and bad.stderr and bad.stdout == PROMPT * 101


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--epq", type=Path, default=Path("./epq"))
    epq = parser.parse_args().epq.resolve()
    with tempfile.TemporaryDirectory(prefix="epq-io-check-") as directory:
        root = Path(directory)
        source = root / "io.epq"
        source.write_text(PROGRAM)
        run([epq, "jit", source], root / "input")
        binary = root / "native"
        subprocess.run([epq, "build", source, "-o", binary], check=True)
        run([binary], root / "input")
    print("Interactive and batch I/O checks passed on JIT and standalone")


if __name__ == "__main__":
    main()
