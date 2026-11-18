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

The program prints `<< >> 14` followed by a newline. Each input expression emits a `<< ` prompt, including when input comes from a file or pipe. An interactive caller can wait for the prompt before sending an integer. During long bursts of buffered input, the runtime groups pending prompt writes and drains them before waiting for more input, printing a result, or exiting.

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

Both compilers resolve names and track which integers, lists and function literals can reach each expression. The JIT emits ARM64 instructions directly, keeps frequently accessed locals in registers, and inlines small functions with a known target. For eligible loops it checks the full index range once, then accesses the list's element array directly. If the guard fails, execution follows the ordinary loop with per-access bounds checks.

The standalone compiler emits C++ with native parameters and return values where their types are known, giving Apple Clang the whole program to optimize. It can also accelerate long counted loops whose body updates a few integers using addition, subtraction and multiplication by constants. It models one iteration as an affine transformation and applies repeated squaring to compute the final values in logarithmically many steps, with EPQ's 32-bit wraparound.

A mutable list normally has a stable address, holds up to two items inline, and stores each value in one tagged machine word. A list with a single initializer and no observable aliases can use a more compact representation. For example, the sieve writes only `0` and `1` into its flag list, so each flag fits in one byte. Its list of primes is only measured for length, so a counter is enough. The compiler also recognizes simple loops that append a repeated value or a contiguous slice. Bounds checks, mutations through aliases, and evaluation of the appended expressions retain their EPQ semantics.

Local variable definitions inside expressions have dedicated storage, so an assignment such as `4 + (x := 7)` preserves both the sum and the new binding. Integer arithmetic wraps to 32 bits on overflow. Decimal literals above 2,147,483,647 saturate at that value, matching the original Windows scanner, and a leading minus is a separate operator. Ordered comparisons preserve the language's operand order across compilers. The upstream grammar also recognizes class and member syntax. Its implementation provides no execution rules for these forms. Evaluating them in CodexEPQ raises a diagnostic.

The runtime retains ordinary list objects until the program exits. When they grow, shorter element arrays return to a pool for reuse. The pool retains its peak allocation, and programs that repeatedly discard entire lists can use memory proportional to the number created. Recursive calls use the machine stack. The build reserves a 512 MiB stack for the JIT process and standalone executables, with physical pages allocated as needed.

## Test and benchmark

Run the conformance and interactive I/O tests from this checkout. For differential tests, point the scripts at an unmodified clone of the upstream repository. Python 3 is needed for these checks.

```sh
make test
git clone https://github.com/BoomydayCoder/EPQ.git ../EPQ &&
    git -C ../EPQ checkout --detach 3253cbc688dd76157cd20be70c7f93b2453ccfbd
python3 bench/build_original.py ../EPQ -o /tmp/epq-original
python3 tests/conformance.py --epq ./epq --original /tmp/epq-original
python3 tests/differential.py --epq ./epq --original /tmp/epq-original --upstream ../EPQ
python3 bench/measure.py ../EPQ --compiler ./epq --original /tmp/epq-original --json /tmp/epq-results.json
```

The differential runner also accepts `--family optimizer` for generated programs that stress list aliasing, function calls, and index arithmetic.

The benchmark checks the complete output of each original example, including input prompts. It times execution inside the engines, source-to-exit wall time, and standalone compilation separately. To time an individual execution, set `EPQ_TIMING=1` when running the JIT or a built program. Its runtime writes elapsed nanoseconds to standard error. The benchmark report includes the inputs, compiler flags, baseline adjustments, and per-example results. [Benchmark report](bench/README.md) · [Raw timing samples](bench/results.json)
