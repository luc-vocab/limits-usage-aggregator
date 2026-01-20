# Pre-Trade Risk Aggregation Engine - Build Aliases
# Source this file: source .bash_aliases

# Docker image name
AGGREGATOR_IMAGE="aggregator-build"

# Build Docker image (run once or after Dockerfile changes)
alias docker-image='docker build -t $AGGREGATOR_IMAGE .'

# Build and run all tests
alias build-test='docker run --rm -v $(pwd):/src -w /src $AGGREGATOR_IMAGE bazel test //tests:test_runner --test_output=all'

# Build only (no tests)
alias build='docker run --rm -v $(pwd):/src -w /src $AGGREGATOR_IMAGE bazel build //...'

# Run tests only
alias test='docker run --rm -v $(pwd):/src -w /src $AGGREGATOR_IMAGE bazel test //tests:test_runner'

# Run specific test suite (usage: test-filter fix, test-filter aggregation, test-filter integration)
test-filter() {
    docker run --rm -v $(pwd):/src -w /src $AGGREGATOR_IMAGE bazel test //tests:test_runner --test_arg=--filter="$1"
}

# Clean build artifacts
alias clean='docker run --rm -v $(pwd):/src -w /src $AGGREGATOR_IMAGE bazel clean'
