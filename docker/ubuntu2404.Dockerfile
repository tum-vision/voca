# Dockerfile for registry.freedesktop.org/mateosss/basalt and generating .deb package

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC

# TODO: Add --no-install-recommends to reduce image size (requires going step-by-step)
RUN apt update && \
  apt upgrade -y && \
  apt install -y \
    gcc \
    g++ \
    cmake \
    mold \
    git \
    clang-format-15 \
    cmake-format \
    unzip \
    python3-pip \
    python3-venv \
    libtbb-dev \
    libeigen3-dev \
    libglew-dev \
    ccache \
    libgl1-mesa-dev \
    libjpeg-dev \
    libpng-dev \
    liblz4-dev \
    libbz2-dev \
    libboost-regex-dev \
    libboost-filesystem-dev \
    libboost-date-time-dev \
    libboost-program-options-dev \
    libgtest-dev \
    libopencv-dev \
    libfmt-dev \
    ninja-build \
    libepoxy-dev \
    jq \
    tree \
    zip \
    7zip \
    curl && \
  export CLANGD_URL=$(curl -s https://api.github.com/repos/clangd/clangd/releases/latest | grep -oP "\"browser_download_url.*clangd-linux-(\d|\.)+\.zip\"" | cut -d ":" -f 2,3 | tr -d \") && \
    echo "Setting up clangd-tidy" && \
    echo "CLANGD_URL=" $CLANGD_URL && \
    curl -L --silent --show-error --fail $CLANGD_URL -o clangd-linux.zip && \
    unzip -q clangd-linux.zip && \
    rm clangd-linux.zip && \
    mv clangd* /clangd && \
    python3 -m venv /venv && \
    . /venv/bin/activate && \
    pip install clangd-tidy && \
  rm -rf /var/lib/apt/lists/* && \
  apt autoremove -y
