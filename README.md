# CodexEPQ

CodexEPQ runs EPQ programs natively on Apple Silicon. EPQ is an imperative language that uses symbols for input, output, control flow, and function definitions. Its values are signed integers, mutable lists, and first-class functions. This repository contains an independent implementation of [BoomydayCoder's EPQ language](https://github.com/BoomydayCoder/EPQ).

## Build and run

On an Apple Silicon Mac with macOS 26 and the Xcode Command Line Tools installed, run the following commands. The build uses the system C++ compiler and includes the parser tables.

```sh
git clone https://github.com/BoomydayCoder/CodexEPQ.git
cd CodexEPQ
make -j4

cat > /tmp/total.epq <<'EPQ'
sum := (values) => {
    total := 0;
    (v : values) @ total := total + v;
    => total;
};
>> sum([<<, 4, 7]);
EPQ

printf '3\n' | ./epq /tmp/total.epq
```

The program prints `<< >> 14` followed by a newline. The first `<< ` is the prompt for the piped input.

| Command | When to use it |
| --- | --- |
| `./epq program.epq` | Run a source file. A cache hit uses its optimized executable. On a miss, the compiler generates ARM64 code in memory and runs it immediately. |
| `./epq jit program.epq` | Run the in-memory compiler explicitly. |
| `./epq native program.epq` | Compile with Apple Clang, cache the executable, and run it. Later runs reuse the cached binary. |
| `./epq build program.epq -o myprogram` | Build a standalone optimized executable. Run it directly with `./myprogram`. |
| `./epq emit program.epq` | Inspect the C++ generated for the standalone build. |

The native cache uses the per-user macOS temporary directory. Set `EPQ_CACHE_DIR` to choose another location. A source path of `-` reads program text from standard input. Compile on the target Mac so Clang can select instructions for its CPU.

## Reading EPQ

An assignment such as `x := 4` defines or updates a binding and evaluates to the assigned value. The `=` operator compares values. Curly braces group statements and introduce a local scope. Functions can read and update globals, call themselves recursively, and take other functions as arguments. A function returns with `=> expression;` and yields integer zero if control reaches its end.

```text
twice := (x) => { => x * 2; };
apply := (f, x) => { => f(x); };
numbers := [2, 3];
alias := numbers;
alias :+ apply(twice, numbers[0]);
>> numbers;                 // >> [2, 3, 4]
>> -alias;                  // >> 4
>> |numbers|;               // >> 2
```

Lists share their contents across assignments and function calls. `:+` appends one item and yields the list. Unary `-` removes and yields the final item. `|x|` gives the length of a list or the absolute value of an integer. Lists support zero-based indexing, indexed assignment, structural equality, and lexicographic ordering.

The loop in the first example visits the list in index order and checks its current length each time. EPQ also has a while form and a counted loop. A question mark runs a statement conditionally, and a colon introduces the optional alternative.

```text
(i := 0; i < 3; i := i + 1;) @ {
    (i = 1) ? >> 100; : >> i;
}
// Prints 0, 100, and 2 on successive output lines.
```

Conditions treat zero and an empty list as false. `&` and `|` short-circuit and return the selected operand. Binary expressions evaluate left to right. A function call evaluates its arguments from left to right, then evaluates the callee.

## Implementation and compatibility

Both execution paths use the same parser, scope resolution, and runtime. The standalone compiler tracks the values and list elements that can reach each expression. Where the type or callee is known, it emits scalar arithmetic, direct calls, and ordinary native control flow that Clang can inline and optimize. It can also turn a loop that appends the same value a known number of times into a single list expansion and fill. Dynamic values fit in one machine word. Lists keep a stable identity as they grow and store their first two elements inline.

Local variable definitions inside expressions have dedicated storage, so an assignment such as `4 + (x := 7)` preserves both the sum and the new binding. Integer arithmetic wraps to 32 bits on overflow. Decimal literals above 2,147,483,647 saturate at that value, matching the original Windows scanner, and a leading minus is a separate operator. Ordered comparisons preserve the language's operand order across compilers. The upstream grammar also recognizes class and member syntax. Its implementation provides no execution rules for these forms. Evaluating them in CodexEPQ raises a diagnostic.

The runtime keeps list allocations until the program exits. Programs that repeatedly discard lists can use memory proportional to the total number of lists created. Recursive calls use the machine stack. The build reserves a 512 MiB stack for the JIT process and standalone executables, with physical pages allocated as needed.

## Test and benchmark

Run the conformance suite from this checkout. For differential tests, point the scripts at an unmodified clone of the upstream repository. Python 3 is needed for these checks.

```sh
python3 tests/conformance.py --epq ./epq
git clone https://github.com/BoomydayCoder/EPQ.git ../EPQ &&
    git -C ../EPQ checkout --detach 3253cbc688dd76157cd20be70c7f93b2453ccfbd
python3 bench/build_original.py ../EPQ -o /tmp/epq-original
python3 tests/conformance.py --epq ./epq --original /tmp/epq-original
python3 tests/differential.py --epq ./epq --original /tmp/epq-original
python3 bench/measure.py ../EPQ --compiler ./epq --original /tmp/epq-original --json /tmp/epq-results.json
```

The benchmark runs the original example files and checks their complete output, including input prompts. Timings cover program execution, process startup, and standalone compilation. The report specifies inputs, compiler settings, baseline adjustments, and per-example speedups. [Benchmark report](bench/README.md) · [Raw timing samples](bench/results.json)
