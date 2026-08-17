CXX      ?= g++
CXXFLAGS ?= -std=c++20 -g -O1 -Wall -Wextra -Wpedantic
SAN      ?= -fsanitize=address,undefined -fno-omit-frame-pointer

all: server

server: server.cpp
	$(CXX) $(CXXFLAGS) $(SAN) -o $@ $<

release: server.cpp
	$(CXX) $(CXXFLAGS) -O2 -DNDEBUG -o server $<

run: server
	./server 9000

clean:
	rm -f server loadgen

.PHONY: all run release clean

loadgen: loadgen.cpp
	$(CXX) $(CXXFLAGS) -O2 -o $@ $<
