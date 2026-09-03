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

server: server.cpp tick_logic.h
	$(CXX) $(CXXFLAGS) -DNDEBUG -o $@ $<

# Correctness build. Use it at a small --conns to check behaviour, then
# rebuild with `make` before recording any number.
asan: server.cpp tick_logic.h
	$(CXX) -std=c++20 -g -O1 -Wall -Wextra -Wpedantic $(SAN) -o server-asan $<

run: server
	./server 9000

# ---------------------------------------------------------------- server-sds
#
# The second build: the same server on the engine's sds:: primitives, no STL.
# It consumes the engine through an INSTALL PREFIX via find_package — the seam
# server-uring uses — so it needs cmake and an installed engine. `make` above
# stays plain and dependency-free on purpose: it is the published baseline.
#
#   cmake -S ../engine-uring -B ../build/engine -DCMAKE_BUILD_TYPE=Release
#   cmake --build ../build/engine
#   cmake --install ../build/engine --prefix ../build/prefix
#
# Override the prefix with IOURING_NET_PREFIX=/path if it lives elsewhere.
IOURING_NET_PREFIX ?= $(CURDIR)/../build/prefix

sds: server_sds.cpp CMakeLists.txt
	@test -f "$(IOURING_NET_PREFIX)/lib/cmake/iouring_net/iouring_netConfig.cmake" || { \
	  echo "no installed engine at $(IOURING_NET_PREFIX) — install it first:"; \
	  echo "  cmake -S ../engine-uring -B ../build/engine -DCMAKE_BUILD_TYPE=Release"; \
	  echo "  cmake --build ../build/engine && cmake --install ../build/engine --prefix ../build/prefix"; \
	  exit 1; }
	cmake -S . -B build/sds -DCMAKE_BUILD_TYPE=Release -DSERVER_SDS_SANITIZE=OFF \
	      -DCMAKE_PREFIX_PATH="$(IOURING_NET_PREFIX)" >/dev/null
	cmake --build build/sds
	cp build/sds/server-sds ./server-sds

# Correctness build of server-sds. Same rule as `asan`: check, then rebuild
# with `make sds` before recording any number.
sds-asan: server_sds.cpp CMakeLists.txt
	cmake -S . -B build/sds-asan -DCMAKE_BUILD_TYPE=Debug -DSERVER_SDS_SANITIZE=ON \
	      -DCMAKE_PREFIX_PATH="$(IOURING_NET_PREFIX)" >/dev/null
	cmake --build build/sds-asan
	cp build/sds-asan/server-sds-asan ./server-sds-asan

clean:
	rm -f server server-asan server-sds server-sds-asan
	rm -rf build

.PHONY: all run asan sds sds-asan clean
