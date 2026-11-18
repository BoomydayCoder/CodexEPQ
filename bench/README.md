# Measuring the six EPQ examples

Run these commands from the CodexEPQ checkout on an Apple Silicon Mac with Xcode Command Line Tools and Python 3.9 or later. The reference checkout and all six example files are read-only inputs to the benchmark.

```sh
git clone https://github.com/BoomydayCoder/EPQ.git ../EPQ
git -C ../EPQ checkout --detach 3253cbc688dd76157cd20be70c7f93b2453ccfbd
make -j
python3 bench/build_original.py ../EPQ -o /tmp/epq-original
python3 bench/measure.py ../EPQ --compiler ./epq --original /tmp/epq-original --json /tmp/epq-results.json
```

The published run compared two native builds of the original VM in the same rounds. To repeat that comparison on a Mac with GNU g++ 16 installed, run the following after the first four commands above.

```sh
python3 bench/build_original.py ../EPQ --cxx g++-16 -o /tmp/epq-original-gnu
python3 bench/measure.py ../EPQ --compiler ./epq \
  --original /tmp/epq-original-gnu --original-label 'GNU g++ 16.2.0' \
  --alternate-original /tmp/epq-original --alternate-label 'Apple Clang 21.0.0' \
  --json /tmp/epq-results.json
```

## Inputs and correctness

| Example | Workload | Expected result |
| --- | --- | --- |
| Fibonacci | 45, the largest input with every intermediate sum in signed 32-bit range | 1,134,903,170 |
| Eratosthenes | Sieve through 1,000,000 | 78,498 primes |
| Dijkstra | 2,048 vertices, 8,192 undirected edges, positive weights, seed 73, 32 unreachable vertices | Full distance vector, independently computed with a Python priority queue |
| Bubble sort, heap sort, merge sort | Each example generates its own 5,000 integers with the upstream parameters | Complete sorted list, independently generated and sorted with Python |

Before timing an example the harness verifies the exit status and compares stdout byte for byte against the independently calculated transcript for the original VM, the source command and the precompiled executable. The comparisons include the `<< ` prompts, `>> ` prefix, spaces, brackets and newlines. Each JSON record identifies the exact source, input and expected output by SHA-256.

## Timing boundaries

All reported times are medians. By default each program gets two untimed warmup rounds and eleven measurement rounds. Engines run sequentially with a rotating launch order. Program stdin comes from an ordinary file, stdout goes to `/dev/null`, and the full output formatting and prompt writes execute. The harness collects the execution clocks in separate instrumented runs so their diagnostic writes do not inflate the wall times.

| Metric | What is inside the interval |
| --- | --- |
| VM execution | The original VM's existing `-t` clock around `vm.run()`, reported in whole microseconds |
| Native execution | The standalone program's `EPQ_TIMING=1` clock around the EPQ entry point through the output flush, reported in nanoseconds |
| VM wall | Launching the original, reading and parsing the source, producing bytecode, running it and exiting |
| JIT wall | Launching `epq SOURCE` with an empty native cache, reading and compiling the source in memory, running it and exiting |
| Native wall | Launching an already compiled, standalone Mach-O program, running it and exiting |
| Build | Running `epq build SOURCE -o FILE` through a completed standalone binary, including code generation, Apple Clang and linking. Median of three fresh builds; the OS file cache is warm. |

The JIT never uses the build undertaken for the native timings. The source command uses a fresh temporary cache directory for this run. Process timings use a monotonic nanosecond clock without subtracting launch overhead. A single Fibonacci(45) computation takes only tens of microseconds including its input and output. Its process-wall measurement depends strongly on launch latency.

## Reference build on macOS

The baseline uses the optimization flags from the upstream README, including `-O3 -flto -march=native -DNDEBUG`. Apple's `g++` is normally Apple Clang. On this Mac, both Apple Clang 21 and GNU g++ 16.2 built from untouched sources return zero for `1 < 2` and `2 > 1`. Their operand evaluation order reverses two VM stack comparisons. The build script compiles a temporary copy of `vm.cc` with the right operand popped first at those two instructions. It also marks the old Flex scanner as C++ and permits its legacy `register` qualifiers. The source and examples in the reference checkout stay byte-identical. The builder checks integer and list comparisons and jumps longer than 255 bytes before accepting the executable.

To try an installed GNU C++ compiler, pass `--cxx /path/to/g++` to `build_original.py`. To reproduce the unadapted behavior, add `--unmodified`. On both tested compilers the script then reports the failed operand-order check.

## Results on Apple M5

The 17 November 2026 run used a 24 GiB Apple M5, macOS 26.6 (25G72), SDK 26.5, Apple Clang 21.0.0 (clang-2100.1.1.101), and GNU g++ 16.2.0. Both reference builds passed the operand-order checks and all four variants produced identical stdout for every workload. The tables give medians in milliseconds. The engine and process clocks use independent trials, which accounts for the Clang bubble-sort execution median slightly exceeding its process median. [Raw samples and SHA-256 digests](results.json)

### Execution inside each engine

| Example | Original GNU | Original Clang | AOT | Speedup vs GNU | Speedup vs Clang |
| --- | ---: | ---: | ---: | ---: | ---: |
| Dijkstra | 82.132 | 98.004 | 27.268 | 3.01× | 3.59× |
| Eratosthenes | 356.6 | 436.1 | 15.398 | 23.16× | 28.32× |
| Fibonacci | 0.075 | 0.050 | 0.041 | 1.82× | 1.21× |
| Bubble sort | 2,152.3 | 2,744.2 | 14.207 | 151.50× | 193.16× |
| Heap sort | 23.837 | 30.128 | 0.607 | 39.29× | 49.66× |
| Merge sort | 20.758 | 23.835 | 1.859 | 11.16× | 12.82× |

### Launch to exit

| Example | Original GNU | Original Clang | Source JIT | JIT vs GNU / Clang | Standalone AOT | AOT vs GNU / Clang | AOT build |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Dijkstra | 112.4 | 111.4 | 41.095 | 2.74× / 2.71× | 36.921 | 3.04× / 3.02× | 196.8 |
| Eratosthenes | 385.6 | 447.4 | 28.354 | 13.60× / 15.78× | 23.029 | 16.75× / 19.43× | 147.5 |
| Fibonacci | 27.460 | 8.816 | 8.505 | 3.23× / 1.04× | 8.385 | 3.27× / 1.05× | 117.7 |
| Bubble sort | 2,169.9 | 2,726.1 | 102.6 | 21.15× / 26.57× | 21.734 | 99.84× / 125.43× | 135.7 |
| Heap sort | 50.859 | 37.962 | 11.108 | 4.58× / 3.42× | 8.408 | 6.05× / 4.51× | 162.4 |
| Merge sort | 50.956 | 35.186 | 12.072 | 4.22× / 2.91× | 11.282 | 4.52× / 3.12× | 151.0 |

For Fibonacci, the launch-to-exit interquartile ranges were 26.81–29.40 ms (GNU VM), 7.58–9.50 ms (Clang VM), 7.98–9.03 ms (source JIT), 7.82–8.75 ms (standalone AOT). The tiny loop shares its execution interval with one input prompt, one integer read and one print. The process measurements also include the C++ runtime loader. GNU uses libstdc++ and Apple Clang uses the system libc++.
