CXX      ?= g++
CXXFLAGS ?= -std=c++20 -g -O2 -Wall -Wextra -Wpedantic -Isrc

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:.cpp=.o)

all: loadgen

loadgen: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJ:.o=.d)

# Sanitised build. Not for measurement — ASan roughly halves throughput, which
# means the numbers describe the sanitiser. Use it to check correctness at a
# small --conns, then measure with the default build.
asan: $(SRC)
	$(CXX) -std=c++20 -g -O1 -Wall -Wextra -Wpedantic -Isrc \
	    -fsanitize=address,undefined -fno-omit-frame-pointer -o loadgen-asan $^

clean:
	rm -f loadgen loadgen-asan src/*.o src/*.d

.PHONY: all asan clean
