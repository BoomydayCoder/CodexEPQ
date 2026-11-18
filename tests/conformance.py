#!/usr/bin/env python3
"""Check complete EPQ stdout, including interactive prompts, against fixed expectations."""

import argparse
import difflib
import os
import random
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Case:
    name: str
    program: str
    stdout: str
    stdin: str = ""
    corrected: bool = False  # The upstream executable is not an oracle for this case.
    error: bool = False  # These cases require an orderly diagnostic and a nonzero exit.


CASES = [
    Case("empty", "// no statements\n{};", ""),
    Case("integer_operators", """
        >> [14 / -4, -14 / 4, -14 / -4, 20 - 6 - 3, 24 / 2 / 3];
        >> [!2 = 1, !0, 1 | 0 & 0, 1 + 2 * 3, 1 < 2 = 1];
        >> [|(-17)|, |[[], [1]]|];
    """, ">> [-3, -3, 3, 11, 4]\n>> [1, 1, 1, 7, 1]\n>> [17, 2]\n"),
    Case("comparison", """
        >> [1 < 2, 1 > 2, 3 <= 3, 3 >= 3, 3 != 3, [] = 0];
        >> [[1,[2,3]] = [1,[2,3]], [0] < [1], [2] < [2,0], [[2]] > [[1,0]]];
    """, ">> [1, 0, 1, 1, 0, 0]\n>> [1, 1, 1, 1]\n"),
    Case("input_prompts", "a := <<; >> [a, <<, <<];", "<< << << >> [-8, 2, 0]\n", "  -8\n+2 0\n"),
    Case("short_circuit", """
        calls := 0;
        inc := () => { calls := calls + 1; => 77; };
        >> [4 | inc(), 0 & inc(), 0 | inc(), 2 & inc(), calls];
        >> [0 & (1 / 0), 5 | [][0], 0 | [1,2], 1 & []];
    """, ">> [4, 0, 77, 77, 2]\n>> [0, 5, [1, 2], []]\n"),
    Case("alias_pop", """
        a := [1,2]; both := [a, a];
        >> (a :+ 3) = both[1];
        >> -both[0];
        >> both[1][1] := 90;
        >> [a, both];
        b := (a :+ 7) :+ 8;
        >> [|a|, -b, a];
    """, ">> 1\n>> 3\n>> 90\n>> [[1, 90], [[1, 90], [1, 90]]]\n>> [4, 8, [1, 90, 7]]\n"),
    Case("eval_order", """
        log := [];
        mark := (x) => { log :+ x; => x; };
        >> mark(9) - mark(3);
        pack := (x,y) => { => [x,y]; };
        select := () => { mark(8); => pack; };
        >> select()(mark(1), mark(2));
        vals := [0,0];
        base := () => { mark(6); => vals; };
        base()[mark(1)] := mark(7);
        >> [vals, log];
    """, ">> 6\n>> [1, 2]\n>> [[0, 7], [9, 3, 1, 2, 8, 6, 1, 7]]\n"),
    Case("callee_rebinding", """
        plus := (x) => { => 10 + x; };
        minus := (x) => { => 10 - x; };
        fn := plus;
        change := () => { fn := minus; => 2; };
        >> fn(change());
        fn := plus;
        >> (fn(change()) + fn(3));
    """, ">> 8\n>> 15\n"),
    Case("subscript_target_rebinding", """
        previous := [3]; current := previous;
        rebind := () => { current := [10]; => 41; };
        current[0] := rebind();
        >> [previous, current];
        hold := current;
        which := () => { current := [0]; => 0; };
        current[which()] := 20;
        >> [hold, current];
    """, ">> [[41], [10]]\n>> [[20], [0]]\n"),
    Case("scopes", """
        early := (n) => { count := count + n; => late(count); };
        late := (n) => { => n * 3; }; count := 2;
        { temporary := 5; { temporary := 7; } >> early(temporary); }
        { temporary := 11; >> [temporary, count]; }
        (1) ? (0) ? >> 1; : >> 2;
        (0) ? >> 3; : >> 4;
    """, ">> 27\n>> [11, 9]\n>> 2\n>> 4\n"),
    Case("top_level_binding_rules", """
        i := 0;
        (i := i + 1) < 0;
        (i < 3) @ t := (i := i + 1);
        (s := 1) ? >> [t, s]; : ;
        (v : [7,9]) @ last := v;
        >> last;
    """, ">> [3, 1]\n>> 9\n"),
    Case("function_values", """
        factory := () => { => (x) => { => x + 1; }; };
        f := factory(); g := factory(); h := (x) => { => x + 1; };
        >> [f = g, f = h, f != 0, [f] = [g], [h] = [f]];
        table := [g,h]; >> table[1](9);
        apply := (func, x) => { => func(x); };
        >> apply(f, 10);
        fall := () => { ; }; >> fall();
    """, ">> [1, 0, 1, 1, 0]\n>> 10\n>> 11\n>> 0\n"),
    Case("function_format", """
        f := (x) => { => x + 7; };
        >> f;
        >> [0, f, 1];
    """, ">> function: {\n0:\tOP_GET_LOCAL 0\n2:\tOP_CONST 7\n4:\tOP_ADD\n5:\tOP_RETURN\n6:\tOP_NULL\n7:\tOP_RETURN\n}\n\n"
         ">> [0, function: {\n0:\tOP_GET_LOCAL 0\n2:\tOP_CONST 7\n4:\tOP_ADD\n5:\tOP_RETURN\n6:\tOP_NULL\n7:\tOP_RETURN\n}\n, 1]\n"),
    Case("recursion", """
        even := (n) => { (n = 0) ? => 1; => odd(n - 1); };
        odd := (n) => { (n = 0) ? => 0; => even(n - 1); };
        tree := (n) => { (n <= 1) ? => [n]; => [tree(n-1), tree(n-2)]; };
        >> [even(9), odd(9), tree(3)];
    """, ">> [0, 1, [[[1], [0]], [1]]]\n"),
    Case("list_rebinding", """
        x := 2;
        bump := (n, l) => { n := 7; l[0] := n; l := [9]; => l; };
        a := [3]; b := bump(x, a); >> [a, b, x];
        rows := [[], []]; rows[0] :+ 5;
        >> rows;
    """, ">> [[7], [9], 2]\n>> [[5], []]\n"),
    Case("live_range", """
        source := [1,2]; alias := source;
        seen := []; calls := 0;
        choose := () => { calls := calls + 1; => source; };
        (item : choose()) @ {
            seen :+ item;
            (item = 1) ? { alias :+ 3; source := [40]; }
            (item = 2) ? { alias[2] := 8; }
        }
        >> [seen, source, calls];
        iter := 6; (iter : []) @ >> 300;
        >> iter;
        (iter : [4,7]) @ ; >> iter;
        (i : seen) @ (j : [1]) @ { (i = 2) ? >> [i,j]; }
    """, ">> [[1, 2, 8], [40], 1]\n>> 0\n>> 7\n>> [2, 1]\n"),
    Case("return_loops", """
        find := (n) => { (i := 0; i < n; i := i + 1;) @ {
            (i = 3) ? { => i * 2; }
        } };
        >> [find(3), find(4)];
        => <<; >> 22;
    """, ">> [0, 6]\n<< ", "4\n"),
    Case("mixed_list_elements", """
        first := (l) => { => l[0]; };
        apply := (x) => { => x + 5; };
        a := [12]; alias := a; box := [a];
        >> first(a) + 3;
        mutate := (target, val) => { target[0] := val; };
        mutate(box[0], [7,9]);
        >> [a[0][1], -first(alias), a[0]];
        mutate(alias, apply);
        >> first(a)(3);
        >> alias[0] = apply;
    """, ">> 15\n>> [9, 9, [7]]\n>> 8\n>> 1\n"),
    Case("polymorphic_calls", """
        identity := (x) => { => x; };
        recursive := (n, item) => { (n) ? => recursive(n-1, item); => item; };
        >> [identity(5), recursive(4,[2]), identity(identity)(2)];
        fs := [identity, (x) => { => [x]; }];
        choose := (n) => { => fs[n]; };
        >> [choose(1)(7), choose(0)([4])];
        changed := 9; (1) ? changed := [0,1]; >> changed[1];
    """, ">> [5, [2], 2]\n>> [[7], [4]]\n>> 1\n"),
    Case("alias_range_shrink", """
        ls := [4,6,8,10]; seen := [];
        (item : ls) @ { seen :+ item; -ls; }
        >> [seen, ls];
        l := []; l :+ l; l :+ 3;
        node := l[0]; >> [|node|, -node, |l|]; -l;
    """, ">> [[4, 6], [4, 6]]\n>> [2, 3, 1]\n"),
    Case("conditions_as_values", """
        f := () => { => 0; };
        ([]) ? >> 0; : >> 1;
        ([0]) ? >> 2;
        (f) ? >> 3;
        >> [[] | 4, [0] & 5, f & 6, [1] | 7];
    """, ">> 1\n>> 2\n>> 3\n>> [4, 5, 6, [1]]\n", corrected=True),
    Case("fresh_loop_binding", """
        { i := 0; (i < 2) @ {
            (i = 0) & (x := 7); >> x; i := i + 1;
        } }
    """, ">> 7\n", corrected=True, error=True),
    Case("argument_shadowing", "name := 3; f := (name) => { => name; };", "", error=True),
    Case("function_does_not_capture", "{ n := 9; f := () => { => n; }; >> f(); }", "", error=True),
    Case("late_undefined_global", "f := () => { => n; }; >> f(); n := 2;", "", error=True),
    Case("short_circuit_suppresses_local", "{ 0 & (x := 7); >> x; }", "", corrected=True, error=True),
    Case("short_circuit_suppresses_global", "0 & (x := 7); >> x;", "", error=True),
    Case("out_of_range_write", "a := [0]; a[1] := <<;", "<< ", "8", error=True),
    Case("invalid_call", "f := (x) => { => x; }; f(<<, <<);", "<< << ", "1 2", error=True),
    Case("unreachable_class_syntax", """
        (0) ? >> #{};
        f := () => { .sample; }; >> f;
        >> 1;
    """, ">> function: {\n0:\tOP_POP\n1:\tOP_NULL\n2:\tOP_RETURN\n}\n\n>> 1\n"),
    Case("unsupported_members", ".sample := 12;", "", corrected=True, error=True),
    Case("unsupported_classes", ">> #{};", "", corrected=True, error=True),
    Case("windows_decimal_conversion", """
        >> [2147483647, 2147483648, -2147483648, 99999999999999999999];
        >> [2147483647 + 1, (2147483647 + 1) / -1, |(2147483647 + 1)|];
    """, ">> [2147483647, 2147483647, -2147483647, 2147483647]\n"
         ">> [-2147483648, -2147483648, -2147483648]\n", corrected=True),
    Case("fill_loop_aliases", """
        dest := []; child := [5]; counter := 20;
        (counter := 0; counter <= 1; counter := counter + 1;) @ dest :+ child;
        child[0] := 9; >> [dest, counter];
        (counter := 8; counter < 3; counter := counter + 1;) @ dest :+ future;
        future := 7; >> [dest, counter];
        (counter := 0; counter < 2; counter := 1 + counter;) @ dest :+ [];
        dest[2] :+ 30; >> [dest, counter];
    """, ">> [[[9], [9]], 2]\n>> [[[9], [9]], 8]\n>> [[[9], [9], [30], []], 2]\n"),
    Case("deep_non_tail_recursion", """
        f := (n) => {
            (n = 0) ? => [0,0];
            v := f(n - 1);
            v[0] := (v[0] + n + v[1] / 2) / 3;
            v[1] := n; => v;
        };
        >> f(350000);
    """, ">> [262499, 350000]\n"),
    Case("inline_definitions", """
        { >> 4 + (x := 7); >> x; }
        { >> [(a := 4), (b := a+2), a*b]; }
        f := (a,b) => { => [a,b]; };
        { >> f((x := 3), (y := x+1)); >> [x,y]; }
        { n := 0; ((current := n + 1) < 4) @ { >> current; n := current; } }
    """, ">> 11\n>> 7\n>> [4, 6, 24]\n>> [3, 4]\n>> [3, 4]\n>> 1\n>> 2\n>> 3\n", corrected=True),
]


def run(command, case, environment):
    return subprocess.run(command, input=case.stdin.encode(), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=environment, timeout=60)


def differences(expected, actual):
    return "".join(difflib.unified_diff(expected.splitlines(keepends=True),
                                        actual.splitlines(keepends=True),
                                        fromfile="expected", tofile="actual"))


def arithmetic_cases():
    """Keep each generated program within the original bytecode constant-pool width."""
    rng = random.Random(134807)
    for batch in range(4):
        expressions, answers = [], []
        for _ in range(12):
            value = rng.randrange(-5, 8)
            expr = str(value)
            for _ in range(3):
                rhs = rng.randrange(1, 8)
                token = rng.choice(("+", "-", "*", "/", "<", ">=", "="))
                if token == "+":
                    value += rhs
                elif token == "-":
                    value -= rhs
                elif token == "*":
                    value *= rhs
                elif token == "/":
                    value = int(value / rhs)
                elif token == "<":
                    value = int(value < rhs)
                elif token == ">=":
                    value = int(value >= rhs)
                else:
                    value = int(value == rhs)
                expr = f"({expr} {token} {rhs})"
            expressions.append(expr)
            answers.append(value)
        yield Case(f"generated_arithmetic_{batch + 1}", ">> [" + ",".join(expressions) + "];",
                   f">> {answers}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--epq", type=Path, default=Path("./epq"))
    parser.add_argument("--backend", choices=("both", "jit", "native"), default="both",
                        help="candidate engines to check (default: both)")
    parser.add_argument("--original", type=Path, help="also check the reference-compatible cases")
    parser.add_argument("--only", help="substring filter for case names")
    args = parser.parse_args()
    failures, count = [], 0
    with tempfile.TemporaryDirectory(prefix="epq-conformance-") as tmp:
        environment = dict(os.environ, EPQ_CACHE_DIR=str(Path(tmp, "cache")))
        modes = ("jit", "native") if args.backend == "both" else (args.backend,)
        for case in [*CASES, *arithmetic_cases()]:
            if args.only and args.only not in case.name:
                continue
            source = Path(tmp, case.name + ".epq")
            source.write_text(case.program)
            engines = [(mode, [str(args.epq.resolve()), mode]) for mode in modes]
            if args.original and not case.corrected:
                engines.append(("original", [str(args.original.resolve())]))
            for name, engine in engines:
                p = run([*engine, str(source)], case, environment)
                actual = p.stdout.decode(errors="replace")
                status_ok = (p.returncode > 0 and p.stderr) if case.error else p.returncode == 0
                if not status_ok or actual != case.stdout:
                    failures.append(f"{name} {case.name} (exit {p.returncode})\n"
                                    + p.stderr.decode(errors="replace") + differences(case.stdout, actual))
            count += 1
    for failure in failures:
        print(failure, file=sys.stderr)
    if not count:
        parser.error("no case matched --only")
    if failures:
        print(f"{len(failures)} failed checks across {count} cases", file=sys.stderr)
        return 1
    print(f"{count} conformance cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
