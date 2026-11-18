CXX := /usr/bin/clang++
NATIVE_CXX := /usr/bin/clang++
NATIVE_FLAGS := -std=c++17 -O3 -DNDEBUG -fwrapv -mcpu=native
CPPFLAGS += -Isrc -Iruntime -Ibuild
CXXFLAGS += $(NATIVE_FLAGS) -Wall -Wextra

SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(patsubst src/%.cpp,build/%.o,$(SOURCES))
HEADERS := $(wildcard src/*.hpp src/*.inc runtime/*.hpp)
RUNTIME_HEADERS := $(patsubst runtime/%,build/runtime/%,$(wildcard runtime/*.hpp))

.PHONY: all clean test
all: epq

epq: $(OBJECTS) build/epq_runtime.o build/runtime.pch
	$(CXX) $(LDFLAGS) $(OBJECTS) build/epq_runtime.o -Wl,-dead_strip,-stack_size,0x20000000 -o $@

build/%.o: src/%.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

build/epq_runtime.o: runtime/epq_runtime.cpp | build
	$(NATIVE_CXX) $(CPPFLAGS) $(NATIVE_FLAGS) -MMD -MP -c $< -o $@

build/runtime.pch: build/runtime/epq_runtime.hpp $(RUNTIME_HEADERS) | build
	$(NATIVE_CXX) $(NATIVE_FLAGS) -Ibuild/runtime -x c++-header $< -o $@

build/runtime/%.hpp: runtime/%.hpp | build/runtime
	cp $< $@

build/main.o: build/build_id.hpp

# Fingerprint the generator, runtime, toolchain and target used to produce executables.
build/build_id.hpp: $(SOURCES) $(HEADERS) runtime/epq_runtime.cpp Makefile | build
	@{ /usr/bin/shasum -a 256 $^; $(NATIVE_CXX) --version; \
	    $(NATIVE_CXX) -mcpu=native -### -x c++ -c /dev/null 2>&1; } | \
	    /usr/bin/shasum -a 256 | awk '{print "#define EPQ_BUILD_ID \"" $$1 "\""}' > $@.tmp
	@mv $@.tmp $@

build build/runtime:
	mkdir -p $@

test: epq
	python3 tests/conformance.py --epq ./epq

clean:
	$(RM) -r build epq

-include $(OBJECTS:.o=.d) build/epq_runtime.d
