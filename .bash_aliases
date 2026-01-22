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

# Generate code coverage report (HTML output in ./coverage)
coverage() {
    $DOCKER_RUN $AGGREGATOR_IMAGE bash -c '
        # Run coverage to generate gcda files
        bazel --output_base=/bazel-cache/output --install_base=/bazel-cache/install coverage --repository_cache=/bazel-cache/repos --nocache_test_results //tests:test_runner

        # Find the latest TestRunner sandbox with gcda files
        SANDBOX_DIR=$(find /bazel-cache/output/sandbox/sandbox_stash/TestRunner -name "*.gcda" 2>/dev/null | head -1 | xargs dirname 2>/dev/null)
        GCNO_DIR="/bazel-cache/output/execroot/_main/bazel-out/k8-dbg/bin/tests/_objs/test_runner"

        # Copy gcda files next to gcno files
        if [ -n "$SANDBOX_DIR" ]; then
            cp "$SANDBOX_DIR"/*.gcda "$GCNO_DIR/" 2>/dev/null || true
        fi

        # Capture coverage with lcov
        cd /bazel-cache/output/execroot/_main
        lcov --capture --directory "$GCNO_DIR" --output-file /bazel-cache/coverage_raw.info --ignore-errors source --ignore-errors gcov 2>/dev/null

        # Filter to only include project source files
        lcov --extract /bazel-cache/coverage_raw.info "*/src/*" "*/tests/*" --output-file /bazel-cache/coverage_filtered.info --ignore-errors source 2>/dev/null

        # Fix paths for genhtml
        sed "s|/proc/self/cwd/|/src/|g" /bazel-cache/coverage_filtered.info > /bazel-cache/coverage.info

        # Generate HTML report
        genhtml /bazel-cache/coverage.info -o /src/coverage --ignore-errors source

        echo ""
        echo "Coverage report generated: coverage/index.html"
    '
}
