# Linux build environment for fitzel.
#
# Not a cross toolchain: Docker Desktop runs this container in WSL2, so the
# compiler inside builds natively for Linux x86_64. Ubuntu 22.04 on purpose --
# glibc is only forward compatible, and a binary linked against an older one
# starts on more distributions than one built on the newest.
#
# GCC 12 rather than jammy's default 11: C++20 support in 11 still has holes.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git cmake ninja-build pkg-config \
        gcc-12 g++-12 \
        python3 python3-jinja2 \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols \
        libgl1-mesa-dev libasound2-dev \
    && apt-get clean

ENV CC=gcc-12 CXX=g++-12

# The source is bind-mounted from Windows and owned by someone else as far as
# git inside the container is concerned; without this VersionGen gets no
# commit count and stamps build 0.
RUN git config --global --add safe.directory '*'

WORKDIR /src