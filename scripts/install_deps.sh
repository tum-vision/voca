#!/usr/bin/env bash
##
## BSD 3-Clause License
##
## This file is part of the Basalt project.
## https://gitlab.com/VladyslavUsenko/basalt.git
##
## Copyright (c) 2019-2021, Vladyslav Usenko and Nikolaus Demmel.
## All rights reserved.
##

set -euo pipefail

run_as_root() {
	if [[ ${EUID} -eq 0 ]]; then
		"$@"
	elif command -v sudo >/dev/null 2>&1; then
		sudo "$@"
	else
		echo "sudo is not available." >&2
		exit 1
	fi
}

if [[ "$OSTYPE" == "darwin"* ]]; then
	brew install boost opencv cmake pkg-config lz4 clang-format tbb glew eigen ccache fmt llvm ffmpeg mesa
else
	if [[ ! -r /etc/os-release ]]; then
		echo "Cannot determine the Linux distribution: /etc/os-release is unavailable." >&2
		exit 1
	fi

	# shellcheck disable=SC1091
	source /etc/os-release

	case "${ID:-}" in
		fedora)
			run_as_root dnf install -y \
				gcc gcc-c++ cmake ninja-build mold git \
				tbb-devel eigen3-devel glew-devel ccache \
				libjpeg-turbo-devel libpng-devel lz4-devel bzip2-devel \
				boost-regex boost-filesystem boost-date-time boost-program-options \
				gtest-devel opencv-devel fmt-devel libepoxy-devel \
				ffmpeg-free ffmpeg-free-devel mesa-libGL-devel
			;;
		ubuntu | debian)
			# VOCA defaults to GCC and libstdc++ on Linux. LLVM's libc++ and
			# libunwind packages are not required and their versioned runtime
			# names differ between Ubuntu releases (notably Ubuntu 24.04).
			run_as_root apt-get install -y \
				gcc g++ cmake ninja-build mold git pkg-config \
				libtbb-dev libeigen3-dev libglew-dev ccache \
				libjpeg-dev libpng-dev liblz4-dev libbz2-dev \
				libboost-regex-dev libboost-filesystem-dev \
				libboost-date-time-dev libboost-program-options-dev \
				libgtest-dev libopencv-dev libfmt-dev libwayland-bin libepoxy-dev \
				ffmpeg libavcodec-dev libavformat-dev libavutil-dev \
				libswscale-dev libgl1-mesa-dev
			;;
		*)
			echo "Unsupported Linux distribution '${ID:-unknown}'." >&2
			echo "Supported distributions are Ubuntu, Debian, and Fedora." >&2
			exit 1
			;;
	esac
fi
