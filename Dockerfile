FROM gcc:13.2.0

WORKDIR /src

# Install Bazel dependencies and Bazel
RUN apt-get update && apt-get install -y \
    apt-transport-https \
    curl \
    gnupg \
    lcov \
    && curl -fsSL https://bazel.build/bazel-release.pub.gpg | gpg --dearmor > /usr/share/keyrings/bazel-archive-keyring.gpg \
    && echo "deb [arch=amd64 signed-by=/usr/share/keyrings/bazel-archive-keyring.gpg] https://storage.googleapis.com/bazel-apt stable jdk1.8" | tee /etc/apt/sources.list.d/bazel.list \
    && apt-get update && apt-get install -y bazel \
    && rm -rf /var/lib/apt/lists/*

# Default command
CMD ["bazel", "build", "//..."]
