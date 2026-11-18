# Measuring the six EPQ examples

Run these commands from the CodexEPQ checkout on an Apple Silicon Mac with Xcode Command Line Tools and Python 3.9 or later. The reference checkout and all six example files are read-only inputs to the benchmark.

```sh
git clone https://github.com/BoomydayCoder/EPQ.git ../EPQ
git -C ../EPQ checkout --detach 3253cbc688dd76157cd20be70c7f93b2453ccfbd
make -j
python3 bench/build_original.py ../EPQ -o /tmp/epq-original
python3 bench/measure.py ../EPQ --compiler ./epq --original /tmp/epq-original --json /tmp/epq-results.json
```

The published comparison interleaves two native builds of the original VM and two versions of CodexEPQ. On a Mac with GNU g++ 16 installed, run the following after the first four commands above.

```sh
python3 bench/build_original.py ../EPQ --cxx g++-16 -o /tmp/epq-original-gnu
git clone https://github.com/BoomydayCoder/CodexEPQ.git ../CodexEPQ-previous
git -C ../CodexEPQ-previous checkout --detach fd775dee24c7982f94c411b1cc28814ccd1c269b
make -C ../CodexEPQ-previous -j
python3 bench/measure.py ../EPQ --compiler ./epq \
  --previous-compiler ../CodexEPQ-previous/epq \
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

All reported times are medians. By default each program gets two untimed warmup rounds and eleven measurement rounds. Engines run sequentially with a rotating launch order. Program stdin comes from an ordinary file and stdout goes to `/dev/null`. Every engine formats and writes the full transcript. CodexEPQ can combine prompt writes for integers already buffered in memory and flushes before it waits for more input, prints, or exits. The harness collects the execution clocks in separate instrumented runs so their diagnostic writes do not inflate the wall times.

| Metric | What is inside the interval |
| --- | --- |
| VM execution | The original VM's existing `-t` clock around `vm.run()`, reported in whole microseconds |
| Native execution | The standalone program's `EPQ_TIMING=1` clock around the EPQ entry point through the output flush, reported in nanoseconds |
| JIT execution | The same runtime clock around the generated ARM64 entry point, starting after in-memory compilation |
| VM wall | Launching the original, reading and parsing the source, producing bytecode, running it and exiting |
| JIT wall | Launching `epq SOURCE` with an empty native cache, reading and compiling the source in memory, running it and exiting |
| Native wall | Launching an already compiled, standalone Mach-O program, running it and exiting |
| Build | Running `epq build SOURCE -o FILE` through a completed standalone binary, including code generation, Apple Clang and linking. Median of three fresh builds; the OS file cache is warm. |

The JIT never uses the build undertaken for the native timings. The source command uses a fresh temporary cache directory for this run. Process timings use a monotonic nanosecond clock without subtracting launch overhead. A single Fibonacci(45) computation takes only tens of microseconds including its input and output. Its process-wall measurement depends strongly on launch latency.

## Reference build on macOS

The baseline uses the optimization flags from the upstream README, including `-O3 -flto -march=native -DNDEBUG`. Apple's `g++` is normally Apple Clang. On this Mac, both Apple Clang 21 and GNU g++ 16.2 built from untouched sources return zero for `1 < 2` and `2 > 1`. Their operand evaluation order reverses two VM stack comparisons. The build script compiles a temporary copy of `vm.cc` with the right operand popped first at those two instructions. It also marks the old Flex scanner as C++ and permits its legacy `register` qualifiers. The source and examples in the reference checkout stay byte-identical. The builder checks integer and list comparisons and jumps longer than 255 bytes before accepting the executable.

To try an installed GNU C++ compiler, pass `--cxx /path/to/g++` to `build_original.py`. To reproduce the unadapted behavior, add `--unmodified`. On both tested compilers the script then reports the failed operand-order check.

## Results on Apple M5

The 17 November 2026 run used a 24 GiB Apple M5, macOS 26.6 (25G72), SDK 26.5, Apple Clang 21.0.0 (clang-2100.1.1.101), and GNU g++ 16.2.0. All six variants produced identical stdout for every workload. Times are medians in milliseconds. The execution and process samples come from independent runs. [Raw samples and SHA-256 digests](results.json)

### Execution inside each engine

| Example | Original GNU | Original Clang | JIT | AOT | AOT vs GNU | AOT vs Clang |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Dijkstra | 87.022 | 108.5 | 5.675 | 4.453 | 19.54× | 24.36× |
| Eratosthenes | 373.6 | 468.3 | 8.142 | 4.761 | 78.46× | 98.35× |
| Fibonacci | 0.088 | 0.053 | 0.035 | 0.046 | 1.90× | 1.15× |
| Bubble sort | 2,242.3 | 2,855.8 | 59.646 | 15.093 | 148.57× | 189.22× |
| Heap sort | 24.318 | 29.851 | 1.232 | 0.625 | 38.93× | 47.79× |
| Merge sort | 20.841 | 24.171 | 1.679 | 1.290 | 16.16× | 18.74× |

### Launch to exit

| Example | Original GNU | Original Clang | Source JIT | JIT vs GNU / Clang | Standalone AOT | AOT vs GNU / Clang | AOT build |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Dijkstra | 121.4 | 117.2 | 18.708 | 6.49× / 6.26× | 16.701 | 7.27× / 7.02× | 391.4 |
| Eratosthenes | 410.3 | 473.1 | 20.574 | 19.94× / 23.00× | 13.551 | 30.28× / 34.91× | 404.6 |
| Fibonacci | 29.960 | 9.302 | 9.798 | 3.06× / 0.95× | 8.323 | 3.60× / 1.12× | 222.5 |
| Bubble sort | 2,234.0 | 2,792.9 | 66.466 | 33.61× / 42.02× | 24.646 | 90.64× / 113.32× | 249.6 |
| Heap sort | 52.777 | 39.063 | 11.588 | 4.55× / 3.37× | 9.131 | 5.78× / 4.28× | 277.4 |
| Merge sort | 51.578 | 34.454 | 11.227 | 4.59× / 3.07× | 9.893 | 5.21× / 3.48× | 247.2 |

### Comparison with the previous release

The same rounds included a fresh build of the previous release at commit `fd775de`. The tuned compiler was built from `0b42c0a`. Both source commands used an empty native cache for the timed runs. [Tuned source](https://github.com/BoomydayCoder/CodexEPQ/tree/0b42c0a9415e08b51f46b0dd02d79612db9551d4) · [Previous release](https://github.com/BoomydayCoder/CodexEPQ/tree/fd775dee24c7982f94c411b1cc28814ccd1c269b)

| Example | Previous JIT execution | Current JIT execution | Execution speedup | Previous JIT wall | Current JIT wall | Wall speedup | Previous AOT execution | Current AOT execution |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Dijkstra | 31.821 | 5.675 | 5.61× | 43.439 | 18.708 | 2.32× | 31.340 | 4.453 |
| Eratosthenes | 19.517 | 8.142 | 2.40× | 30.808 | 20.574 | 1.50× | 18.298 | 4.761 |
| Fibonacci | 0.038 | 0.035 | 1.08× | 9.192 | 9.798 | 0.94× | 0.045 | 0.046 |
| Bubble sort | 98.000 | 59.646 | 1.64× | 109.5 | 66.466 | 1.65× | 15.043 | 15.093 |
| Heap sort | 1.491 | 1.232 | 1.21× | 11.711 | 11.588 | 1.01× | 0.665 | 0.625 |
| Merge sort | 2.362 | 1.679 | 1.41× | 13.298 | 11.227 | 1.18× | 1.949 | 1.290 |

For Fibonacci, the launch-to-exit interquartile ranges were 28.38–31.17 ms (GNU VM), 7.63–9.69 ms (Clang VM), 8.27–9.67 ms (previous source JIT), 9.46–10.21 ms (current source JIT), 7.90–9.25 ms (standalone AOT). Its execution interval includes an input prompt, an integer read and a print. Process timings include loading libstdc++ for the GNU build or libc++ for the Clang builds.
