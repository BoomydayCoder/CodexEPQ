#!/usr/bin/env python3
"""Compare seeded, terminating EPQ programs with the GCC-compatible reference VM."""

import argparse
import difflib
import random
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

HELPERS = """
audit := [];
note := (x) => { audit :+ x; => x; };
plus := (x,y) => { => x+y; };
minus := (x,y) => { => x-y; };
selected := plus;
flip := (x) => { (selected = plus) ? selected := minus; : selected := plus; => note(x); };
dispatch := (choice,x,y) => { => choice(x,y); };
bundle := (x,y) => { => [x,y]; };
id := (x) => { => x; };
store := (xs,i,x) => { => xs[i] := x; };
factory := (tag) => { (tag) ? => store; => (xs,i,x) => { xs[i] := [x]; => x; }; };
exchange := (xs,i,j) => { saved := xs[i]; xs[i] := xs[j]; xs[j] := saved; };
nested := (depth,x) => { (depth <= 0) ? => x; => [nested(depth-1,x), x]; };
"""


def scenario(seed):
    rng = random.Random(seed)
    def small():
        return str(rng.randint(-6, 6))

    # The only pop of the integer buffer immediately undoes an append. All indices
    # into that buffer stay in its first three elements, including after aliasing.
    # Repeated assignments target previously defined locals, avoiding the old
    # VM's inline-definition stack bugs. Arithmetic stays in the signed 32-bit range.
    index = lambda: rng.choice(("0", "1", "2"))
    integer = lambda: rng.choice(("a", "b", "arg", small(), f"buf[{index()}]", "(|buf|)"))
    integer_bin = lambda: f"({integer()} {rng.choice(('+', '-', '<', '>', '>=', '<=', '=', '!='))} {integer()})"
    lines = [
        f"trial{seed} := (arg) => {{",
        "audit := []; selected := plus;",
        f"a := {small()}; b := {small()}; buf := [a, arg, b];",
        "alias := buf; other := [arg, a, b]; pair := [buf, other];",
        "state := [arg, 0]; ops := [plus,minus];",
        "mixed := a; record := [];",
    ]
    # Construct every statement family, randomize the order and the expressions
    # within it. A higher-order call, a rebind, and reads through escaped aliases
    # thus interact with different histories in each reproducible seed.
    def actions():
        return [
            f"a := {integer_bin()};",
            f"b := {integer()} * {rng.choice((-1, 0, 1))};",
            f"alias[{index()}] := note({integer()});",
            f"exchange(id(pair[{rng.randrange(2)}]), {index()}, {index()});",
            f"record :+ bundle(note({integer()}), note({integer()}));",
            f"record :+ selected(flip({integer()}), flip({integer()}));",
            f"record :+ dispatch(ops[{rng.randrange(2)}], note({integer()}), {integer()});",
            f"ops[{rng.randrange(2)}] := selected;",
            f"record :+ ({integer()} {rng.choice(('&', '|'))} note({integer()}));",
            f"(arg {rng.choice(('>', '<=', '='))} {small()}) ? alias := other; : alias := buf;",
            f"({integer_bin()}) ? mixed := bundle({integer()}, {integer()}); : mixed := {integer()};",
            f"record :+ [mixed = {integer()}, mixed, id(mixed)];",
            f"pair[{rng.randrange(2)}] := alias;",
            f"buf :+ {integer()}; record :+ -buf;",
            f"state[note({rng.randrange(2)})] := note({integer_bin()});",
            f"record :+ (buf := id(pair[{rng.randrange(2)}]));",
            f"(iter := 0; iter < {rng.randint(0, 4)}; iter := iter+1;) @ "
            f"{{ alias[{index()}] := alias[{index()}] + iter; }}",
            f"step := {rng.randint(0, 3)}; (step) @ {{ record :+ step; step := step-1; }}",
            f"scratch := [{small()},{small()}]; rounds := 0; "
            f"(elem : scratch) @ {{ record :+ elem; (rounds = 0) ? scratch :+ {small()}; rounds := rounds+1; }}",
            f"cells := [[{small()}],[{small()}]]; writer := factory(arg); "
            f"slot := id(cells[{rng.randrange(2)}]); writer(slot,0,bundle({integer()},{integer()})); "
            f"record :+ [slot[0],cells,|slot[0]|];",
            f"record :+ nested({rng.randrange(4)}, id(state));",
            f"record :+ [pair[0] = pair[1], pair[0] < pair[1], pair[0] >= pair[1]];",
            f"record :+ [(a := {integer()}), (b := {integer()}), a, b];",
        ]
    ops = actions()
    rng.shuffle(ops)
    lines.extend(ops)
    lines.extend(("=> [a,b,buf,alias,other,pair,record,audit,state,mixed];", "};"))
    return "\n".join(lines)


def optimizer_scenario(seed, wrapped):
    """Stress reuse of checked cells under aliasing and mutation in a taken branch."""
    rng = random.Random(seed)
    def sub(base, offset):
        return f"{base}[i+{offset}]"

    # A guard may compare the same location under different names. The branch
    # then alters the binding, the affine index, or the data at that address.
    n = rng.randint(4, 8)
    literals = ",".join(str(rng.randint(-32, 32)) for _ in range(n))
    lines = [f"trial{seed} := (arg) => {{",
             f"left := [{literals}]; right := [{literals}]; right[0] := arg;",
             "audit := []; selected := plus; curr := left; al := right;",
             "(arg <= 0) ? al := left; (arg >= 0) ? curr := right;",
             "i := 0; acc := 0; bytevec := [0,2,3]; wide := [10,20,30]; ct := [];"]
    for _ in range(rng.randint(3, 7)):
        a, b = rng.choices(("left", "right", "curr", "al"), k=2)
        x, y = rng.choices(range(n - 1), k=2)
        first, second = sub(a, x), sub(b, y)
        mutation = rng.choice((
            f"exchange({a},i+{x},i+{y});",
            f"{second} := {first} + 1; {first} := {second};",
            f"i := 1-i; {first} := {second} - 1;",
            f"curr := {b}; al := {a}; {sub('curr', x)} := {sub('al', y)};",
            f"store({a},i+{x},store({b},i+{y},{first}));",
            f"{second} := bytevec[i] + wide[i]; bytevec[i] := {rng.randrange(256)};",
        ))
        op = rng.choice((">", "<", ">=", "<=", "=", "!="))
        lines.append(f"({first} {op} {second}) ? {{ {mutation} acc := {first}; }} "
                     f": {{ acc := {second}; }}")
    # Mutating a list used only for its length must still evaluate its item.
    lines += [f"ct :+ note(acc); ct[i-i] := note({rng.randint(-9, 9)});",
              "wide[i] := selected(flip(arg),note(acc)); ct :+ note(wide[i]);",
              "dest := right; (arg >= 0) ? dest := al;",
              f"(p := {rng.randrange(3)}; p < {rng.randrange(3, n + 1)}; p := p+1;) @ dest :+ curr[p];"]
    # Compose equivalent array indices in the modulo-2^32 implementation.
    # The reference runs the offset-free variant to avoid signed overflow.
    wrapping_case = wrapped and seed % 3 == 0
    lines.append(f"q := {2147483647 if wrapping_case else 0};")
    def q_index(offset):
        return f"q+2147483647+{offset + 2}" if wrapping_case else f"q+{offset}"
    x, y = rng.sample(range(n), 2)
    qx, qy = q_index(x), q_index(y)
    lines += [f"(left[{qx}] >= al[{qy}]) ? exchange(left,{qx},{qy});",
              "=> [left,right,al,curr,acc,audit,bytevec[i],wide[i],|ct|]; };"]
    return "\n".join(lines)


def source_for(seeds, generate=scenario):
    return (HELPERS + "\n".join(generate(s) for s in seeds) + "\n"
            + "\n".join(f">> trial{s}(<<);" for s in seeds) + "\n")


def inputs_for(seeds, variant):
    # Each seed takes both sides of the common arg-sign branch across variants.
    if variant < 3:
        return (" ".join(str((-3, 0, 4)[(variant + s) % 3]) for s in seeds) + "\n").encode()
    rng = random.Random(variant + 9949 * seeds[0])
    return (" ".join(str(rng.randint(-6, 6)) for _ in seeds) + "\n").encode()


def execute(command, stdin):
    return subprocess.run(command, input=stdin, capture_output=True, timeout=30)


def fail(source, stdin, message, detail, reference_source=None):
    out = Path(tempfile.mkdtemp(prefix="epq-differential-failure-"))
    (out / "program.epq").write_text(source)
    (out / "input.txt").write_bytes(stdin)
    if reference_source is not None and reference_source != source:
        (out / "reference.epq").write_text(reference_source)
    raise RuntimeError(f"{message}\n{detail}\nReproducer saved in {out}")


def engines_for(mode, candidate, path, artifact):
    engines = {}
    if mode in ("source", "both"):
        engines["source"] = [candidate, "jit", path]
    if mode in ("build", "both"):
        built = execute([candidate, "build", path, "-o", artifact], b"")
        if built.returncode:
            fail(path.read_text(), b"", "native build failed", built.stderr.decode(errors="replace"))
        engines["native"] = [artifact]
    return engines


def edge_examples(args, artifact):
    # Each expected result is independent of both compilers.
    examples = {
        "fibonacci": [([-3], -1), ([0], 0), ([1], 1), ([2], 1), ([10], 55)],
        "eratosthenes": [([0], 0), ([1], 0), ([2], 1), ([3], 2), ([4], 2), ([9], 4)],
        "dijkstra": [
            ([1, 0], [0]),
            ([3, 0], [0, -1, -1]),
            ([2, 1, 0, 1, 0], [0, 0]),
            ([4, 4, 0, 1, 9, 0, 1, 2, 1, 2, 3, 0, 0, 1], [0, 2, 5, -1]),
        ],
    }
    count = 0
    for name, cases in examples.items():
        path = args.upstream.resolve() / "examples" / (name + ".txt")
        engines = engines_for(args.mode, args.epq.resolve(), path, artifact)
        engines["original"] = [args.original.resolve(), path]
        for values, answer in cases:
            stdin = (" ".join(map(str, values)) + "\n").encode()
            expected = ("<< " * len(values) + f">> {answer}\n").encode()
            for label, command in engines.items():
                got = execute(command, stdin)
                if got.returncode or got.stdout != expected:
                    fail(path.read_text(), stdin, f"{label} failed example {name} on {values}",
                         f"expected {expected!r}, got {got.stdout!r}\n" + got.stderr.decode(errors="replace"))
            count += 1
    print(f"{count} upstream example edge inputs passed ({args.mode})")


def suite(args):
    if args.first < 0 or args.seeds < 1 or args.inputs < 1:
        raise ValueError("--first must be nonnegative; --seeds and --inputs must be positive")
    with tempfile.TemporaryDirectory(prefix="epq-differential-") as work:
        path = Path(work, "program.epq")
        artifact = Path(work, "native")
        ref_path = Path(work, "reference.epq")
        candidate, original = args.epq.resolve(), args.original.resolve()
        for start in range(args.first, args.first + args.seeds, 8):
            seeds = range(start, min(start + 8, args.first + args.seeds))
            if args.family == "optimizer":
                source = source_for(seeds, lambda seed: optimizer_scenario(seed, True))
                ref_source = source_for(seeds, lambda seed: optimizer_scenario(seed, False))
            else:
                source = ref_source = source_for(seeds)
            path.write_text(source)
            ref_path.write_text(ref_source)
            engines = engines_for(args.mode, candidate, path, artifact)
            for variant in range(args.inputs):
                stdin = inputs_for(seeds, variant)
                ref = execute([original, ref_path], stdin)
                if ref.returncode:
                    fail(ref_source, stdin, "reference failed", ref.stderr.decode(errors="replace"))
                if ref.stdout.count(b"<< >> ") != len(seeds):
                    fail(source, stdin, "reference did not finish every scenario", repr(ref.stdout))
                for label, command in engines.items():
                    got = execute(command, stdin)
                    if got.returncode or got.stdout != ref.stdout:
                        diff = difflib.unified_diff(
                            ref.stdout.decode(errors="replace").splitlines(),
                            got.stdout.decode(errors="replace").splitlines(),
                            fromfile="reference", tofile=label, lineterm="")
                        # Each terminating scenario prints one line. Keep the
                        # complete batch, which also reproduces type inference.
                        expected_lines, actual_lines = ref.stdout.splitlines(), got.stdout.splitlines()
                        first = next((i for i, pair in enumerate(zip(expected_lines, actual_lines))
                                      if pair[0] != pair[1]), min(len(expected_lines), len(actual_lines)))
                        suspect = min(first, len(seeds) - 1)
                        fail(source, stdin, f"{label} {args.family} mismatch near seed {seeds[suspect]} "
                             f"(input {stdin.split()[suspect].decode()}; batch {start}..{seeds[-1]}), "
                             f"input variant {variant}, exit {got.returncode}",
                             got.stderr.decode(errors="replace") + "\n".join(diff), ref_source)
        if args.upstream:
            edge_examples(args, artifact)
    print(f"{args.seeds} generated scenarios × {args.inputs} inputs passed "
          f"against the reference ({args.mode})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--epq", type=Path, default=ROOT / "epq")
    parser.add_argument("--original", type=Path, required=True,
                        help="binary from bench/build_original.py or the Windows GCC original")
    parser.add_argument("--upstream", type=Path, help="also run edge inputs on the original example files")
    parser.add_argument("--first", type=int, default=1, help="first scenario seed")
    parser.add_argument("--seeds", type=int, default=24, help="number of consecutive seeds")
    parser.add_argument("--inputs", type=int, default=3, help="input variants per seed")
    parser.add_argument("--mode", choices=["source", "build", "both"], default="both")
    parser.add_argument("--family", choices=["general", "optimizer"], default="general",
                        help="generate mixed-language scenarios or stress the optimizer's alias and index reasoning")
    try:
        suite(parser.parse_args())
    except (OSError, subprocess.TimeoutExpired, RuntimeError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
