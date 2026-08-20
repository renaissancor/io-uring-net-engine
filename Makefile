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

# Drift guard for committed generated code: regenerate corpus_data.cpp
# (mkcorpus is deterministic — fixed seed, sorted output) and compare against
# the committed file. The working copy is restored either way; only its mtime
# changes, which costs one rebuild of corpus_data.o.
corpus-check:
	@tmp=$$(mktemp -d) && \
	cp src/corpus_data.cpp $$tmp/committed.cpp && \
	python3 tools/mkcorpus.py >/dev/null && \
	if cmp -s src/corpus_data.cpp $$tmp/committed.cpp; then \
	    cp $$tmp/committed.cpp src/corpus_data.cpp; rm -rf $$tmp; \
	    echo "corpus_data.cpp is in sync with .corpus-src/"; \
	else \
	    mv src/corpus_data.cpp $$tmp/regenerated.cpp; \
	    cp $$tmp/committed.cpp src/corpus_data.cpp; \
	    echo "corpus_data.cpp does not match .corpus-src/ — regenerate with:"; \
	    echo "    python3 tools/mkcorpus.py"; \
	    echo "(regenerated copy kept at $$tmp/regenerated.cpp)"; \
	    exit 1; \
	fi

clean:
	rm -f loadgen loadgen-asan src/*.o src/*.d

.PHONY: all asan clean corpus-check
