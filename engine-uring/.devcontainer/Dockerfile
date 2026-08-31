# .devcontainer/Dockerfile — pinned reproducible build env.
#
# The base image SHOULD be digest-pinned for true reproducibility:
#   docker pull ubuntu:24.04
#   docker inspect --format='{{index .RepoDigests 0}}' ubuntu:24.04
# Then replace `FROM ubuntu:24.04` below with `FROM ubuntu:24.04@sha256:<DIGEST>`.
# Update the digest deliberately at envelope-bump time. See
# docs/07-ci-and-reproducibility.md § "Envelope caveats".
#
# TODO(envelope-bump): pin to a digest before the first release.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      g++-14 clang-18 clang-tidy-18 clang-format-18 lld-18 \
      cmake ninja-build pkg-config linux-libc-dev \
      liburing-dev catch2 libfmt-dev \
      valgrind cppcheck gdb \
      ca-certificates curl git \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
