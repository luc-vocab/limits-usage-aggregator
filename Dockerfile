FROM gcc:13.2.0

WORKDIR /src

# Install any additional tools needed
RUN apt-get update && apt-get install -y \
    make \
    && rm -rf /var/lib/apt/lists/*

# Default command
CMD ["make", "all"]
