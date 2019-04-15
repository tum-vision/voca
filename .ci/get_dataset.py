#!/usr/bin/env python3

"""
Gets a dataset.

This script assumes that the specified working directory lives on a fast
storage. If the specified dataset lives on a different storage, it will wait
until there is enough space on the working storage to uncompress the dataset. In
the end it returns the path of the final dataset to use.

It uses a lockfile to ensure that only one instance of the script can be
transferring files between disks to ensure best transfer speeds.

# Two examples: (notice .zip extension when not present)

# If /slow_disk/euroc/MH_01_easy.zip exists, and assuming CWD is /fast_disk/work
# Get /slow_disk/euroc/MH_01_easy.zip, uncompress it to
# /fast_disk/work/MH_01_easy, and output "/fast_disk/work/MH_01_easy"
get_dataset.py /slow_disk/euroc/MH_01_easy .

# If /fast_disk/euroc/MH_01_easy/ exists, and assuming CWD is /fast_disk/work
# Do not copy anything and output "/fast_disk/euroc/MH_01_easy"
get_dataset.py /fast_disk/euroc/MH_01_easy .

"""

import os
import sys
import json
import time
import math
import argparse
import shutil

from subprocess import run, PIPE
from pathlib import Path

PADDING_KB = 50 * 2**20  # Padding in KB (50 GB)
WHILE_LOCK_SLEEP_SECONDS = 1  # Sleep time betweek lock checks
WHILE_SPACE_WAIT_SECONDS = 1  # Sleep time between space checks


def shout(command, silence=False):
    "Executes command in shell and returns its stdout"
    if not silence:
        print(command)
    result = run(command, shell=True, check=False, stdout=PIPE)
    stdout = result.stdout.decode("utf-8")
    return stdout, result.returncode


def get_mount_point(path):
    """Get the source mount point of a path using findmnt."""
    path = os.path.abspath(path)
    stdout, _ = shout(f"findmnt -T {path} --json")
    data = json.loads(stdout)
    # On docker executor CI you can get e.g., "/dev/sda1[/hdd/monado-slam-datasets/M_monado_datasets/MG_reverb_g2]"
    # While on shell executor CI you get just "/dev/sda1", the following split() works for both
    return data["filesystems"][0]["source"].split("[")[0]


def get_file_size_kb(path):
    """Return the size of a file in kilobytes"""
    size_bytes = os.stat(path).st_size
    size_kb = math.ceil(size_bytes / 1024)
    return size_kb


def get_available_kb(path):
    """Return available space (in KB) on the given path."""
    usage = shutil.disk_usage(path)
    return usage.free // 1024


def wait_space_for_file(file_path, working_dir, padding_kb=PADDING_KB):
    "Wait until enough space on the working directory for the file to be uncompressed"
    required_kb = get_file_size_kb(file_path) + padding_kb
    prev_avail_gb = 0
    while True:
        available_kb = get_available_kb(working_dir)
        avail = available_kb / 2**20  # Convert to GB
        if available_kb < required_kb:
            perc = 100 * available_kb / required_kb
            req = required_kb / 2**20  # Convert to GB
            if avail - prev_avail_gb > 1:  # Print each GB of progress
                print(f"[{perc:.2f}%] Waiting for {req:.2f} GB, but {avail:.2f} GB avail")
                prev_avail_gb = avail
            time.sleep(WHILE_SPACE_WAIT_SECONDS)
        else:
            print(f"Sufficient space available ({avail:.2f} GB), continuing...")
            break


def parse_args():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "dataset_path",
        type=Path,
        help="Path to the dataset file to uncompress (without .zip extension).",
    )
    parser.add_argument(
        "working_dir",
        type=Path,
        help="Fast working directory where the dataset will be uncompressed.",
    )
    parser.add_argument(
        "--lock_file",
        type=str,
        default="/tmp/uncompression.lock",
        help="Path to the lock file.",
    )
    return parser.parse_args()


def main():
    """Main function to handle dataset uncompression."""
    args = parse_args()
    dataset_path = args.dataset_path
    working_dir = args.working_dir
    lock_file = args.lock_file

    zip_path = dataset_path.with_suffix(".zip")

    working_mount_point = get_mount_point(working_dir)
    print(f"{working_mount_point=}")
    if not zip_path.exists():
        dataset_mount_point = get_mount_point(dataset_path)
        print(f"{dataset_mount_point=}")
        if dataset_mount_point == working_mount_point:
            print(dataset_path)
            sys.exit(0)
        else:
            print("No zip, mount points differ")
            sys.exit(1)

    # Assume zip file exists from here on
    dataset_mount_point = get_mount_point(zip_path)
    print(f"{dataset_mount_point=}")

    print(f"Uncompressing {zip_path} to {working_dir}")

    # Naive locking (race conditions possible but rare)
    lock_file = Path(lock_file)
    prev_lock_contents = ""
    while lock_file.exists():
        with lock_file.open(encoding="utf-8") as f:
            lock_contents = f.read().strip()
        if lock_contents != prev_lock_contents:
            print(f"Waiting for lockfile to be removed... {lock_file} = {lock_contents}")
            prev_lock_contents = lock_contents
        time.sleep(WHILE_LOCK_SLEEP_SECONDS)

    # Acquire lock
    with lock_file.open("w", encoding="utf-8") as f:
        f.write(f"{zip_path} ")

    try:
        wait_space_for_file(zip_path, working_dir)
        shout(f"7z x -y {zip_path} -o{working_dir}")
        print("New dataset path name is:")
        print(f"{working_dir / dataset_path.name}")
    except Exception as e:
        print(f"Failed to obtain dataset: {e}")

    # Release lock
    lock_file.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
