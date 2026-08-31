CXX      ?= g++
CXXFLAGS ?= -std=c++20 -g -O2 -Wall -Wextra -Wpedantic
SAN      ?= -fsanitize=address,undefined -fno-omit-frame-pointer

# The default build is the one you MEASURE with. ASan roughly halves
# throughput, so a sanitised binary does not describe this server, it
# describes the sanitiser -- and nothing in the output says which one you are
# holding. That trap used to be the default target here: `make` produced the
# sanitised server and `make release` the fast one, the opposite of client-bench's
# convention next door, so the obvious command was the wrong one.
all: server

server: server.cpp
	$(CXX) $(CXXFLAGS) -DNDEBUG -o $@ $<

# Correctness build. Use it at a small --conns to check behaviour, then
# rebuild with `make` before recording any number.
asan: server.cpp
	$(CXX) -std=c++20 -g -O1 -Wall -Wextra -Wpedantic $(SAN) -o server-asan $<

run: server
	./server 9000

clean:
	rm -f server server-asan

.PHONY: all run asan clean
