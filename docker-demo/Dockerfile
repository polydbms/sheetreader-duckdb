# Use Ubuntu as base image
FROM ubuntu:22.04

# Install dependencies
RUN apt-get update && apt-get install -y \
    wget \
    unzip \
    git \
    cmake \
    build-essential \
    ninja-build \
    libssl-dev \
    python3-dev \
    ccache \
    && rm -rf /var/lib/apt/lists/*

# Download and install DuckDB
RUN wget https://github.com/duckdb/duckdb/releases/download/v1.4.2/duckdb_cli-linux-amd64.zip \
    && unzip duckdb_cli-linux-amd64.zip \
    && mv duckdb /usr/local/bin/ \
    && chmod +x /usr/local/bin/duckdb \
    && rm duckdb_cli-linux-amd64.zip

# Create working directory
WORKDIR /workspace

# Copy the Excel file and demo script
COPY test.xlsx /workspace/
COPY demo.sql /workspace/

# Set the entrypoint to bash for interactive use
CMD ["/bin/bash"]
