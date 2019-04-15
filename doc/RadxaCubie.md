# Cross-Compiling Basalt for Radxa Cubie A7Z

This guide was made and tested under Ubuntu 25.10 and crosscompiling to a Radxa
Cubie A7Z running Debian 11 bullseye aarch64. Like the [raspberry
guide](./Raspberry.md) it is inspired by [this excelent
resource](https://tttapa.github.io/Pages/Raspberry-Pi).

## Setup the Radxa Zero 3W:

- Download the [OS image](https://github.com/radxa-build/radxa-cubie-a7z/releases/latest).
- Burn image to microsd with something like [balena etcher](https://etcher.balena.io/).
- Configure ssh and wifi: [edit before.txt and config.txt](https://docs.radxa.com/en/template/sbc/radxa-os/headless)
- Scan local network for radxa (radxa can be slow, use timeout): `sudo nmap -p 22 -Pn --host-timeout 1s 192.168.0.0/24`
- Connect with ssh to the found IP, default user and password is `radxa`.

## Obtain a sysroot

We need to replicate the Radxa's sysroot on our Ubuntu host.

You can download a pre-built sysroot and extract it:

```bash
wget https://gitlab.freedesktop.org/mateosss/basalt-lfs/-/raw/main/bullseye-arm64-sysroot.tar.xz
tar -xvf bullseye-arm64-sysroot.tar.xz # Extracts to: cmake_modules/bullseye-arm64-sysroot
```

<details>
<summary><strong>Alternatively, build the sysroot yourself:</strong></summary>

```bash
export SYSROOT=cmake_modules/bullseye-arm64-sysroot

# Create and copy sysroot
sudo apt install ubuntu-dev-tools
mk-sbuild --arch=arm64 bullseye --debootstrap-mirror=http://deb.debian.org/debian --name=bullseye --skip-proposed --skip-updates --skip-security
su - $USER
mk-sbuild --arch=arm64 bullseye --debootstrap-mirror=http://deb.debian.org/debian --name=bullseye --skip-proposed --skip-updates --skip-security
sudo sbuild-apt bullseye-arm64 apt-get install libtbb-dev libglew-dev libjpeg-dev libpng-dev liblz4-dev libbz2-dev libboost-regex-dev libboost-filesystem-dev libboost-date-time-dev libboost-program-options-dev libgtest-dev libopencv-dev libfmt-dev libepoxy-dev libgl1-mesa-dev
sudo sbuild-apt bullseye-arm64 apt-get autoremove
sudo cp -r /var/lib/schroot/chroots/bullseye-arm64/ "$SYSROOT"
sudo chown -R $USER:$USER "$SYSROOT"

# Note to destroy debootstrap chroots:
# sbuild-destroychroot bullseye-arm64 # list deletion commands
# mount | grep bullseye # list still mounted things
# sudo schroot -e -c bullseye-arm64-XXXX # XXXX comes from mountgrep command

# Fix symlinks
rm "$SYSROOT"/usr/lib/aarch64-linux-gnu/liblapack.so.3
rm "$SYSROOT"/usr/lib/aarch64-linux-gnu/libblas.so.3
rm "$SYSROOT"/usr/lib/aarch64-linux-gnu/libm.so
rm "$SYSROOT"/usr/lib/aarch64-linux-gnu/libpthread.so
ln -s lapack/liblapack.so.3.11.0 "$SYSROOT"/usr/lib/aarch64-linux-gnu/liblapack.so.3
ln -s blas/libblas.so.3.11.0 "$SYSROOT"/usr/lib/aarch64-linux-gnu/libblas.so.3
ln -s ../../../lib/aarch64-linux-gnu/libm.so.6 "$SYSROOT"/usr/lib/aarch64-linux-gnu/libm.so
ln -s ../../../lib/aarch64-linux-gnu/libpthread.so.0 "$SYSROOT"/usr/lib/aarch64-linux-gnu/libpthread.so

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
tar -cJvf bullseye-arm64-sysroot.tar.xz "$SYSROOT"
```

</details>

## Set up cross-compiler

### Option A: Use Ubuntu's cross-compilers

- TODO: Still not tested, but it doesnt work for [Raspberry.md](./Raspberry.md) nor for [RadxaZero.md](./RadxaZero.md).

### Option B (recommended): Use tttapa cross toolchain

```bash
wget https://github.com/tttapa/docker-arm-cross-toolchain/releases/latest/download/x-tools-aarch64-rpi3-linux-gnu-gcc14.tar.xz
tar -xvf x-tools-aarch64-rpi3-linux-gnu-gcc14.tar.xz
```

Send the correct `libstdc++` to the radxa:

```bash
scp x-tools/aarch64-rpi3-linux-gnu/aarch64-rpi3-linux-gnu/sysroot/lib/libstdc++.so.6.0.33 radxa_user@RADXA_IP:~
ssh radxa_user@RADXA_IP << 'EOF'
  sudo mkdir -p /usr/local/lib/aarch64-linux-gnu
  sudo mv libstdc++.so.6.0.33 /usr/local/lib/aarch64-linux-gnu
  sudo ln -s libstdc++.so.6.0.33 /usr/local/lib/aarch64-linux-gnu/libstdc++.so.6
  sudo ldconfig
EOF
```

## Build

```bash
git apply --directory thirdparty/Pangolin/ scripts/aarch64-patches/pangolin.patch # Apply patch to pangolin to find eigen
cmake --preset=radxa-a7z
cmake --build build
cd build && cpack     # Generates .deb package for aarch64 in build/
```

## Run

```bash
sudo apt install -f ./basalt-monado-radxa-bullseye-aarch64.deb
basalt_vio --help
wget https://huggingface.co/datasets/collabora/monado-slam-datasets/resolve/main/M_monado_datasets/MO_odyssey_plus/MOO_others/MOO11_short_3_backandforth.zip
unzip MOO11_short_3_backandforth.zip
cp /usr/local/share/basalt/msdmo_config.json config.json
# It is recommended to reduce noisy features to diminish processing times
sed -i -e 's/\("config.optical_flow_detection_grid_size": \)[0-9]\+/\160/' config.json
sed -i -e 's/\("config.optical_flow_detection_min_threshold": \)[0-9]\+/\120/' config.json
# Run 3 times command to warm up the system
time basalt_vio --show-gui 0 --dataset-path MOO11_short_3_backandforth --config-path config.json --cam-calib /usr/local/share/basalt/msdmo_calib.json
time basalt_vio --show-gui 0 --dataset-path MOO11_short_3_backandforth --config-path config.json --cam-calib /usr/local/share/basalt/msdmo_calib.json
time basalt_vio --show-gui 0 --dataset-path MOO11_short_3_backandforth --config-path config.json --cam-calib /usr/local/share/basalt/msdmo_calib.json
```

## Debug

See <https://tttapa.github.io/Pages/Raspberry-Pi/C++-Development-RPiOS/Debugging.html>.
