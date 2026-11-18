#!/usr/bin/env python3
"""Build the EPQ reference VM against the host SDK without editing its checkout."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("upstream", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--unmodified", action="store_true",
                        help="compile every source directly and check the resulting VM's operand order")
    args = parser.parse_args()
    upstream = args.upstream.resolve()
    output = args.output.resolve()
    if upstream in output.parents:
        parser.error("choose an output path outside the reference checkout")
    output.parent.mkdir(parents=True, exist_ok=True)

    sources = [upstream / name for name in
               ("main.cpp", "compiler.cc", "driver.cc", "exptree.cc", "lex.yy.c",
                "parser.tab.cc", "program.cc", "value.cc", "vm.cc")]
    version = subprocess.check_output([args.cxx, "--version"], text=True)
    print(version.splitlines()[0], flush=True)
    flags = ["-std=c++17", "-O3", "-flto", "-march=native", "-DNDEBUG", "-x", "c++"]
    with tempfile.TemporaryDirectory(prefix="epq-oracle-") as tmp:
        if not args.unmodified:
            # Both tested native compilers reverse 1 < 2 on the original.
            # The temporary adaptation preserves the two VM stack pops and
            # original Value comparison, making their intended order explicit.
            vm = sources[-1].read_text()
            for operator in ("<", ">"):
                orig = f"stk.push_back(Value(pop(){operator}pop()));"
                fixed = f"{{ Value rhs = pop(); stk.push_back(Value(pop() {operator} rhs)); }}"
                if vm.count(orig) != 1:
                    raise RuntimeError("upstream comparison code changed; review baseline adapter")
                vm = vm.replace(orig, fixed)
            sources[-1] = Path(tmp) / "vm.cc"
            sources[-1].write_text(vm)
            flags.extend(["-I", str(upstream)])
            print("Reference compatibility: sequencing the two relational VM pops "
                  "in a temporary translation unit.", flush=True)
        flags.append("-Wno-register")
        if "clang" in version.lower():
            flags.extend(["-Wno-switch", "-Wno-unqualified-std-cast-call", "-Wno-unsequenced"])
        subprocess.run([args.cxx, *flags, *sources, "-o", output], check=True)
        # Also exercise the order of the high/low jump bytes with a >255-byte jump.
        probe = Path(tmp) / "probe.epq"
        probe.write_text(">> 1 < 2; >> 2 > 1; >> [1,2] < [2,1]; "
                         + "(0) ? {" + "0;" * 100 + "} >> 1234;"
                         + "(i:=0; i<3; i:=i+1;) @ >> i;")
        run = subprocess.run([output, probe], capture_output=True, check=True, timeout=10)
        if run.stdout != b">> 1\n>> 1\n>> 1\n>> 1234\n>> 0\n>> 1\n>> 2\n":
            raise RuntimeError(f"reference compiler failed operand/jump ordering check: {run!r}")
    print(output)


if __name__ == "__main__":
    main()
