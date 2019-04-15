# Running Basalt on a Raspberry Pi (4 & 5)

## Running the `.deb` without building

Quick commands to get you up and running on a raspberry pi 4 or 5 running
Raspberry Pi OS. Tested on a pi 400 running debian-based Raspberry Pi OS
bookworm 12.5 aarch64, and on a Raspberry Pi 5 with bookworm 12 (thanks
[KopterBuzz](https://github.com/KopterBuzz)).

```bash
sudo apt update && sudo apt upgrade

wget https://gitlab.freedesktop.org/mateosss/basalt/-/raw/main/scripts/install_deps.sh
sudo chmod +x ./install_deps.sh
sudo ./install_deps.sh

# Update libstdc++ (might not be necessary in future updates of Raspberry Pi OS)
wget https://syncandshare.lrz.de/dl/fiNh64Rj72bKTypBoZSx2o/libstdc%2B%2B.so.6.0.33
sudo mkdir -p /usr/local/lib/aarch64-linux-gnu
sudo mv libstdc++.so.6.0.33 /usr/local/lib/aarch64-linux-gnu
sudo ln -s libstdc++.so.6.0.33 /usr/local/lib/aarch64-linux-gnu/libstdc++.so.6
sudo ldconfig

# Install latest basalt .deb (see for newer versions here: https://gitlab.freedesktop.org/mateosss/basalt/-/releases)
wget https://gitlab.freedesktop.org/mateosss/basalt/-/jobs/80018889/artifacts/raw/basalt-monado-raspberry-bookworm-pi5-aarch64.deb
sudo apt install -f ./basalt-monado-raspberry-bookworm-pi5-aarch64.deb
basalt_vio --help # Check that the binary runs

# Get a dataset
wget https://huggingface.co/datasets/collabora/monado-slam-datasets/resolve/main/M_monado_datasets/MO_odyssey_plus/MOO_others/MOO11_short_3_backandforth.zip
unzip MOO11_short_3_backandforth.zip

# Let's tune the config file slightly for a bit more performance
cp /usr/local/share/basalt/msdmo_config.json config.json
sed -i -e 's/\("config.optical_flow_detection_grid_size": \)[0-9]\+/\140/' config.json
sed -i -e 's/\("config.optical_flow_detection_min_threshold": \)[0-9]\+/\120/' config.json

# Run and time for thrice to get accurate run durations (see the lowest "Total runtime" for ignoring dataset loading)
time basalt_vio --show-gui 0 --dataset-path MOO11_short_3_backandforth --config-path config.json --cam-calib /usr/local/share/basalt/msdmo_calib.json
time basalt_vio --show-gui 0 --dataset-path MOO11_short_3_backandforth --config-path config.json --cam-calib /usr/local/share/basalt/msdmo_calib.json
time basalt_vio --show-gui 0 --dataset-path MOO11_short_3_backandforth --config-path config.json --cam-calib /usr/local/share/basalt/msdmo_calib.json
```

## Cross-Compiling from scratch

This guide was made and tested under Ubuntu 24.04 with crosscompiling to a
raspberry pi 400 running the latest raspberry pi OS (based on debian bookworm
12.5, aarch64). It is inspired by this excellent resource
<https://tttapa.github.io/Pages/Raspberry-Pi/>.

## Obtain a sysroot

We need to replicate the Raspberry Pi's sysroot on our Ubuntu host.

You can download a pre-built sysroot and extract it:

```bash
wget https://gitlab.freedesktop.org/mateosss/basalt-lfs/-/raw/main/bookworm-arm64-sysroot.tar.xz
tar -xvf bookworm-arm64-sysroot.tar.xz # Extracts to: cmake_modules/bookworm-arm64-sysroot
```

<details>
<summary><strong>Alternatively, build the sysroot yourself:</strong></summary>

```bash
export SYSROOT=cmake_modules/bookworm-arm64-sysroot

# Create and copy sysroot
sudo apt install ubuntu-dev-tools
mk-sbuild --arch=arm64 bookworm --debootstrap-mirror=http://deb.debian.org/debian --name=bookworm --skip-proposed --skip-updates --skip-security
su - $USER
mk-sbuild --arch=arm64 bookworm --debootstrap-mirror=http://deb.debian.org/debian --name=bookworm --skip-proposed --skip-updates   --skip-security
sudo sbuild-apt bookworm-arm64 apt-get install libtbb-dev libglew-dev libjpeg-dev libpng-dev liblz4-dev libbz2-dev libboost-regex-dev libboost-filesystem-dev libboost-date-time-dev libboost-program-options-dev libgtest-dev libopencv-dev libfmt-dev libepoxy-dev libgl1-mesa-dev
sudo sbuild-apt bookworm-arm64 apt-get autoremove
sudo cp -r /var/lib/schroot/chroots/bookworm-arm64/ "$SYSROOT"
sudo chown -R $USER:$USER "$SYSROOT"

# Note to destroy debootstrap chroots:
# sbuild-destroychroot bookworm-arm64 # list deletion commands
# mount | grep bookworm # list still mounted things
# sudo schroot -e -c bookworm-arm64-XXXX # XXXX comes from mountgrep command

# Fix symlinks
rm "$SYSROOT"/usr/lib/aarch64-linux-gnu/liblapack.so.3
rm "$SYSROOT"/usr/lib/aarch64-linux-gnu/libblas.so.3
rm "$SYSROOT"/usr/lib/aarch64-linux-gnu/libm.so
ln -s lapack/liblapack.so.3.11.0 "$SYSROOT"/usr/lib/aarch64-linux-gnu/liblapack.so.3
ln -s blas/libblas.so.3.11.0 "$SYSROOT"/usr/lib/aarch64-linux-gnu/libblas.so.3
ln -s ../../../lib/aarch64-linux-gnu/libm.so.6 "$SYSROOT"/usr/lib/aarch64-linux-gnu/libm.so

# Prune sysroot
cd "$SYSROOT"
rm -rf bin/ boot/ build/ dev/ home/ media/ mnt/ opt/ proc/ root/ run/ sbin/ srv/ sys/ tmp/ var/
rm -rf usr/bin usr/games usr/libexec/ usr/sbin/ usr/share/ usr/src/
rm -rf usr/lib/gcc/
rm -rf usr/lib/aarch64-linux-gnu/dri
rm -rf usr/lib/aarch64-linux-gnu/libLLVM-15.so.1
rm -rf usr/lib/aarch64-linux-gnu/libopencv_cvv.a

# Compress for distribution
cd -
tar -cJvf bookworm-arm64-sysroot.tar.xz "$SYSROOT"
```

</details>

## Set up cross-compiler

<details>
<summary><strong>Option A (not working at the moment, but simpler):</strong> Use Ubuntu's cross-compilers</summary>

This is simpler but not ideal due to current libc incompatibility with the latest Raspberry Pi OS.

```bash
sudo apt install g++-14-aarch64-linux-gnu gcc-14-aarch64-linux-gnu gfortran-14-aarch64-linux-gnu
```

To copy the standard C++ library to your Raspberry Pi:

```bash
# Find the shared library (should be /usr/aarch64-linux-gnu/lib/libstdc++.so.6.0.33)
apt-file list libstdc++-14-dev-arm64-cross | grep libstdc++.so

# Send to the Pi
scp /usr/aarch64-linux-gnu/lib/libstdc++.so.6.0.33 rpi_user@RPI_IP:~
ssh rpi_user@RPI_IP bash << 'EOF'
  sudo mkdir -p /usr/local/lib/aarch64-linux-gnu
  sudo mv libstdc++.so.6.0.33 /usr/local/lib/aarch64-linux-gnu
  sudo ln -s libstdc++.so.6.0.33 /usr/local/lib/aarch64-linux-gnu/libstdc++.so.6
  sudo ldconfig
EOF
```

</details>

### Option B (recommended): Use tttapa cross toolchain

```bash
wget https://github.com/tttapa/docker-arm-cross-toolchain/releases/latest/download/x-tools-aarch64-rpi3-linux-gnu-gcc14.tar.xz
tar -xvf x-tools-aarch64-rpi3-linux-gnu-gcc14.tar.xz
```

Send the correct `libstdc++` to the Raspberry Pi:

```bash
scp x-tools/aarch64-rpi3-linux-gnu/aarch64-rpi3-linux-gnu/sysroot/lib/libstdc++.so.6.0.33 pi@RASPBERRY_IP:~
ssh pi@RASPBERRY_IP << 'EOF'
  sudo mkdir -p /usr/local/lib/aarch64-linux-gnu
  sudo mv libstdc++.so.6.0.33 /usr/local/lib/aarch64-linux-gnu
  sudo ln -s libstdc++.so.6.0.33 /usr/local/lib/aarch64-linux-gnu/libstdc++.so.6
  sudo ldconfig
EOF
```

## Build

```bash
git apply --directory thirdparty/Pangolin/ scripts/aarch64-patches/pangolin.patch # Apply patch to pangolin to find eigen
cmake --preset=rpi4   # or use --preset=rpi5 depending on target
cmake --build build
cd build && cpack     # Generates .deb package for aarch64 in build/
```

## Run

```bash
sudo apt install -f ./basalt-monado-raspberry-bookworm-pi4-aarch64.deb
basalt_vio --help
wget https://huggingface.co/datasets/collabora/monado-slam-datasets/resolve/main/M_monado_datasets/MO_odyssey_plus/MOO_others/MOO11_short_3_backandforth.zip
unzip MOO11_short_3_backandforth.zip
cp /usr/local/share/basalt/msdmo_config.json config.json
# It is recommended to reduce noisy features to diminish processing times
sed -i -e 's/\("config.optical_flow_detection_grid_size": \)[0-9]\+/\140/' config.json
sed -i -e 's/\("config.optical_flow_detection_min_threshold": \)[0-9]\+/\120/' config.json
# Run 3 times command to warm up the system
time basalt_vio --show-gui 0 --dataset-path MOO11_short_3_backandforth --config-path config.json --cam-calib /usr/local/share/basalt/msdmo_calib.json
time basalt_vio --show-gui 0 --dataset-path MOO11_short_3_backandforth --config-path config.json --cam-calib /usr/local/share/basalt/msdmo_calib.json
time basalt_vio --show-gui 0 --dataset-path MOO11_short_3_backandforth --config-path config.json --cam-calib /usr/local/share/basalt/msdmo_calib.json
```

## Debug

See <https://tttapa.github.io/Pages/Raspberry-Pi/C++-Development-RPiOS/Debugging.html>.
