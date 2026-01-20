CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -O2
DEBUG_FLAGS = -g -DDEBUG
INCLUDES = -Isrc

SRC_DIR = src
TEST_DIR = tests
BIN_DIR = bin

# Source files
SRCS = $(wildcard $(SRC_DIR)/*.cpp) \
       $(wildcard $(SRC_DIR)/**/*.cpp)

# Test files
TEST_SRCS = $(wildcard $(TEST_DIR)/*.cpp)

# Targets
.PHONY: all clean test docker-build docker-test

all: $(BIN_DIR)/test_runner

$(BIN_DIR)/test_runner: $(TEST_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(DEBUG_FLAGS) $(INCLUDES) -o $@ $(TEST_SRCS)

test: $(BIN_DIR)/test_runner
	./$(BIN_DIR)/test_runner

clean:
	rm -rf $(BIN_DIR)/*

# Docker commands
docker-build:
	docker run --rm -v $(PWD):/src -w /src gcc:13.2.0 make all

docker-test:
	docker run --rm -v $(PWD):/src -w /src gcc:13.2.0 make test

docker-clean:
	docker run --rm -v $(PWD):/src -w /src gcc:13.2.0 make clean
