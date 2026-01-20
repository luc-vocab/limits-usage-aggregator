# Pre-Trade Risk Aggregation Engine - Build Aliases
# Source this file: source .bash_aliases

# Docker image name
AGGREGATOR_IMAGE="aggregator-build"

# Docker run options: mount source, run as current user, set HOME and USER for bazel
DOCKER_RUN="docker run --rm -v $(pwd):/src -w /src --user $(id -u):$(id -g) -e HOME=/tmp -e USER=$(id -un)"

# Build Docker image (run once or after Dockerfile changes)
alias docker-image='docker build -t $AGGREGATOR_IMAGE .'

# Build and run all tests
alias build-test='$DOCKER_RUN $AGGREGATOR_IMAGE bazel test //tests:test_runner --test_output=all'

# Build only (no tests)
alias build='$DOCKER_RUN $AGGREGATOR_IMAGE bazel build //...'

# Run tests only
alias test='$DOCKER_RUN $AGGREGATOR_IMAGE bazel test //tests:test_runner'

# Run specific test suite (usage: test-filter fix, test-filter aggregation, test-filter integration)
test-filter() {
    $DOCKER_RUN $AGGREGATOR_IMAGE bazel test //tests:test_runner --test_arg=--filter="$1"
}

# Clean build artifacts
alias clean='$DOCKER_RUN $AGGREGATOR_IMAGE bazel clean'
