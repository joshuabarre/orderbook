CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Isrc
UBSAN     = -fsanitize=undefined -fno-omit-frame-pointer -g -O1
ASAN      = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
HDRS      = src/types.hpp src/id_map.hpp src/level_set.hpp src/order_book.hpp src/map_book.hpp
BIN       = build

.PHONY: all test san asan leaks bench clean

all: $(BIN)/test_book $(BIN)/bench

$(BIN):
	@mkdir -p $(BIN)

$(BIN)/test_book: tests/test_book.cpp tests/naive_book.hpp $(HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_book.cpp

# -O3 and -march=native for the benchmark only: numbers are for this machine.
$(BIN)/bench: bench/bench.cpp bench/workload.hpp $(HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) -O3 -march=native -DNDEBUG -o $@ bench/bench.cpp

test: $(BIN)/test_book
	@./$(BIN)/test_book

# UndefinedBehaviorSanitizer. Clean as of the last run.
san: | $(BIN)
	$(CXX) $(CXXFLAGS) $(UBSAN) -o $(BIN)/test_book_ubsan tests/test_book.cpp
	./$(BIN)/test_book_ubsan

# On macOS 26/27 the ASan runtime deadlocks in its own initializer before
# main() runs, whatever the program. Use this target on Linux; use `make leaks`
# on macOS.
asan: | $(BIN)
	$(CXX) $(CXXFLAGS) $(ASAN) -o $(BIN)/test_book_asan tests/test_book.cpp
	./$(BIN)/test_book_asan

leaks: $(BIN)/test_book
	MallocStackLogging=1 leaks --atExit -- ./$(BIN)/test_book | tail -2

bench: $(BIN)/bench
	@./$(BIN)/bench

clean:
	rm -rf $(BIN)

$(BIN)/itch_extract: tools/itch_extract.cpp bench/replay_format.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) -O3 -o $@ tools/itch_extract.cpp
