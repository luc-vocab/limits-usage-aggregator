# Pre-Trade Risk Aggregation Engine - Build Aliases
# Source this file: source .bash_aliases

# Docker image name
AGGREGATOR_IMAGE="aggregator-build"

# Bazel cache directory (persistent across builds)
BAZEL_CACHE="/volatile_home/luc/temp/risk_engine"

# Docker run options: mount source, mount bazel cache, run as current user
DOCKER_RUN="docker run --rm -v $(pwd):/src -v $BAZEL_CACHE:/bazel-cache -w /src --user $(id -u):$(id -g) -e HOME=/tmp -e USER=$(id -un)"

# Bazel startup options (before command)
BAZEL_STARTUP="bazel --output_base=/bazel-cache/output --install_base=/bazel-cache/install"

# Bazel command options (after command) - repository_cache is a command option
BAZEL_OPTS="--repository_cache=/bazel-cache/repos"

# Build Docker image (run once or after Dockerfile changes)
alias docker-image='docker build -t $AGGREGATOR_IMAGE .'

# Build and run all tests
alias build-test='$DOCKER_RUN $AGGREGATOR_IMAGE $BAZEL_STARTUP test $BAZEL_OPTS //tests:test_runner --test_output=all'

# Build only (no tests)
alias build='$DOCKER_RUN $AGGREGATOR_IMAGE $BAZEL_STARTUP build $BAZEL_OPTS //...'

# Run tests only
alias test='$DOCKER_RUN $AGGREGATOR_IMAGE $BAZEL_STARTUP test $BAZEL_OPTS //tests:test_runner'

# Run specific test suite (usage: test-filter fix, test-filter aggregation, test-filter integration)
test-filter() {
    $DOCKER_RUN $AGGREGATOR_IMAGE $BAZEL_STARTUP test $BAZEL_OPTS //tests:test_runner --test_arg=--filter="$1"
}

# Clean build artifacts
alias clean='$DOCKER_RUN $AGGREGATOR_IMAGE $BAZEL_STARTUP clean'
