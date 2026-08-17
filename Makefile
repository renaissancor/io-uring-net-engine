CXX      ?= g++
CXXFLAGS ?= -std=c++20 -g -O2 -Wall -Wextra -Wpedantic

all: loadgen

loadgen: loadgen.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

# Sanitised build. Not for measurement — ASan roughly halves throughput, which
# means the numbers describe the sanitiser. Use it to check correctness at a
# small --conns, then measure with the default build.
asan: loadgen.cpp
	$(CXX) $(CXXFLAGS) -O1 -fsanitize=address,undefined \
	    -fno-omit-frame-pointer -o loadgen-asan $<

clean:
	rm -f loadgen loadgen-asan

.PHONY: all asan clean
