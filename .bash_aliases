# Pre-Trade Risk Aggregation Engine - Build Aliases
# Source this file: source .bash_aliases

# Build and run all tests
alias build-test='docker run --rm -v $(pwd):/src -w /src gcc:13.2.0 bash -c "mkdir -p bin && g++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -g -Isrc -o bin/test_runner tests/test_runner.cpp && ./bin/test_runner"'

# Build only (no tests)
alias build='docker run --rm -v $(pwd):/src -w /src gcc:13.2.0 bash -c "mkdir -p bin && g++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -g -Isrc -o bin/test_runner tests/test_runner.cpp"'

# Run tests only (assumes already built)
alias test='docker run --rm -v $(pwd):/src -w /src gcc:13.2.0 ./bin/test_runner'

# Run specific test suite (usage: test-filter fix, test-filter aggregation, test-filter integration)
test-filter() {
    docker run --rm -v $(pwd):/src -w /src gcc:13.2.0 ./bin/test_runner --filter="$1"
}

# Clean build artifacts
alias clean='rm -rf bin/*'
