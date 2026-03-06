FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gcc \
    g++ \
    make \
    gdb \
    strace \
    wget \
    unzip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
