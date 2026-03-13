FROM --platform=linux/amd64 ubuntu:22.04

RUN apt-get update -qq && \
    apt-get install -y -qq gcc-arm-linux-gnueabihf make && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /work
