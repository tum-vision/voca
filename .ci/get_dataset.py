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
import shlex

from subprocess import run, PIPE
from pathlib import Path

PADDING_KB = 50 * 2**20  # Padding in KB (50 GB)
WHILE_LOCK_SLEEP_SECONDS = 1  # Sleep time betweek lock checks
WHILE_SPACE_WAIT_SECONDS = 1  # Sleep time between space checks
MISMATCH_ALLOWLIST_DATASETS = ["V2_03_difficult", "V1_02_medium"]  # Dataset names allowed to use only shared cam timestamps


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

def compute_framerate(cam_dir):
    """Compute framerate from a EuRoC cam data.csv file."""
    data_csv = Path(cam_dir) / "data.csv"
    assert data_csv.exists(), \
        f"data.csv not found in {cam_dir} — invalid EuRoC camera directory"
    timestamps = get_timestamps_from_csv(data_csv)
    assert len(timestamps) >= 2, \
        f"data.csv in {cam_dir} has fewer than 2 entries — cannot compute framerate"
    # Timestamps are in nanoseconds
    avg_dt_ns = (int(timestamps[-1]) - int(timestamps[0])) / (len(timestamps) - 1)
    return round(1e9 / avg_dt_ns)

def get_euroc_cam_dirs(dataset_path):
    """Fail hard if the expected EuRoC mav0/cam* layout is missing."""
    mav0_path = Path(dataset_path) / "mav0"
    assert mav0_path.is_dir(), \
        f"Expected EuRoC 'mav0' directory not found under {dataset_path}"
    cam_dirs = sorted(mav0_path.glob("cam*"))
    assert cam_dirs, \
        f"No cam* directories found under {mav0_path} — invalid EuRoC dataset"
    return cam_dirs

def get_timestamps_from_csv(data_csv):
    """Return timestamp strings from a EuRoC data.csv in file order."""
    timestamps = []
    with open(data_csv, "r", encoding="utf-8") as f:
        for line in f:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            timestamps.append(stripped.split(",")[0])
    return timestamps

def get_filenames_from_csv(data_csv):
    """Return filename strings from a EuRoC data.csv second column, in file order."""
    filenames = []
    with open(data_csv, "r", encoding="utf-8") as f:
        for line in f:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            filenames.append(stripped.split(",")[1])
    return filenames


def get_video_processing_config(config_path):
    """Return (generate_videos, ffmpeg_options) from config with defaults."""
    options = {}
    generate_videos = False

    with open(Path(config_path), "r", encoding="utf-8") as f:
        config = json.load(f)
    value0 = config.get("value0", {})
    generate_videos = bool(value0.get("config.use_video_frames", False))

    options["use_dataset_framerate"] = bool(value0.get("config.ffmpeg_use_dataset_framerate", False))
    options["framerate"] = int(value0.get("config.ffmpeg_framerate", 30))
    options["encoder"] = str(value0.get("config.ffmpeg_encoder", "libx264"))

    x264opts = value0.get(
        "config.ffmpeg_x264opts",
        "partitions=p8x8,p4x4,i8x8:keyint=1000:me=umh:merange=64:subme=6:bframes=0:ref=1",
    )
    options["x264opts"] = None if x264opts in (None, "") else str(x264opts)

    bitrate = value0.get("config.ffmpeg_x264_bitrate", "500k")
    options["bitrate"] = None if bitrate in (None, "") else str(bitrate)

    options["pix_fmt"] = str(value0.get("config.ffmpeg_pix_fmt", "yuv420p"))

    return generate_videos, options

def recreate_euroc_with_shared_timestamps(dataset_path, video_base, shared_timestamps, cam_dirs):
    """Recreate EuRoC under video_base with only shared cam timestamps in data.csv files."""
    dataset_path = Path(dataset_path)
    video_base = Path(video_base)

    print(f"Recreating filtered EuRoC dataset at {video_base}")
    if video_base.exists():
        shutil.rmtree(video_base)
    # Skip copying cam*/data image folders.
    def _ignore_cam_image_dirs(src, names):
        if Path(src).name.startswith("cam") and Path(src).parent.name == "mav0":
            return {"data"} & set(names)
        return set()
        
    shutil.copytree(dataset_path, video_base, ignore=_ignore_cam_image_dirs)

    for cam_dir in cam_dirs:
        cam_name = cam_dir.name
        orig_csv = cam_dir / "data.csv"
        new_csv = video_base / "mav0" / cam_name / "data.csv"

        header_lines = []
        filtered_rows = []
        with open(orig_csv, "r", encoding="utf-8") as f:
            for line in f:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    header_lines.append(line)
                else:
                    ts = stripped.split(",")[0]
                    if ts in shared_timestamps:
                        filtered_rows.append(line)

        print(f"  {cam_name}: {len(filtered_rows)} rows kept (from {orig_csv.name})")
        with open(new_csv, "w", encoding="utf-8") as f:
            f.writelines(header_lines)
            f.writelines(filtered_rows)

    return video_base

def build_ffmpeg_concat_file(filenames, original_data_dir, output_path, framerate):
    """Write an ffmpeg concat file that points to exact PNG paths.

    The 'ffconcat version 1.0' header and per-entry 'duration' lines are required
    by the concat demuxer for still images, ensuring correct sequential timestamps
    and preventing frames from being dropped.
    """
    frame_duration = 1.0 / framerate
    with open(output_path, "w", encoding="utf-8") as f:
        f.write("ffconcat version 1.0\n\n")
        for filename in filenames:
            png_path = (Path(original_data_dir) / filename).resolve()
            assert png_path.exists(), \
                f"PNG listed in data.csv not found on disk: {png_path}"
            f.write(f"file '{png_path}'\n")
            f.write(f"duration {frame_duration}\n")
    return output_path


def output_videos(working_dir, dataset_path, device, encoding_options):
    """Encode PNGs from a EuRoC-format dataset into per-camera videos.

    We always feed ffmpeg from the original cam*/data PNG paths using a concat
    text file. If camera timestamps do not match and dataset is allowlisted, we
    recreate EuRoC metadata under video_base with filtered data.csv files.

    Returns:
        (video_base, new_dataset_path)
          video_base: output directory for per-camera videos, or None on failure.
          new_dataset_path: recreated filtered EuRoC path, else None.
    """
    dataset_path = Path(dataset_path)
    dataset_name = dataset_path.name
    video_base = Path(working_dir) / "videos" / device / dataset_name

    # Capture original image dirs once; these stay the source of PNG frames.
    original_cam_dirs = get_euroc_cam_dirs(dataset_path)
    original_data_dirs = {}
    for cam_dir in original_cam_dirs:
        data_dir = cam_dir / "data"
        assert data_dir.is_dir(), \
            f"EuRoC image directory not found: {data_dir}"
        original_data_dirs[cam_dir.name] = data_dir

    # CSV paths per cam; these may be redirected to filtered copies later.
    csv_per_cam = {cam_dir.name: cam_dir / "data.csv" for cam_dir in original_cam_dirs}
    for cam_name, csv_path in csv_per_cam.items():
        assert csv_path.exists(), \
            f"data.csv missing from EuRoC camera directory: {csv_path.parent}"

    # Read timestamps from each camera CSV.
    timestamps_per_cam = {
        cam_name: get_timestamps_from_csv(csv_path)
        for cam_name, csv_path in csv_per_cam.items()
    }

    filenames_per_cam = {
        cam_name: get_filenames_from_csv(csv_path)
        for cam_name, csv_path in csv_per_cam.items()
    }

    # Check whether camera timestamp sets match.
    timestamp_sets = {cam: set(ts_list) for cam, ts_list in timestamps_per_cam.items()}
    all_equal = len(set(frozenset(s) for s in timestamp_sets.values())) == 1

    new_dataset_path = None  # Set only for mismatch + allowlist datasets.

    if not all_equal:
        counts = {cam: len(ts) for cam, ts in timestamps_per_cam.items()}
        assert dataset_name in MISMATCH_ALLOWLIST_DATASETS, (
            f"Timestamp mismatch across cameras for dataset '{dataset_name}': {counts}. "
            f"Add it to MISMATCH_ALLOWLIST_DATASETS if this is intentional."
        )

        print(f"WARNING: timestamp mismatch across cameras: {counts}")
        print(f"WARNING: '{dataset_name}' is allowlisted — recreating filtered EuRoC dataset")

        shared_timestamps = set.intersection(*timestamp_sets.values())
        assert shared_timestamps, \
            f"No shared timestamps across all cameras for dataset '{dataset_name}'"

        new_dataset_path = recreate_euroc_with_shared_timestamps(
            dataset_path, video_base, shared_timestamps, original_cam_dirs
        )

        # Use filtered CSVs from recreated dataset; PNGs stay at original paths.
        filtered_cam_dirs = get_euroc_cam_dirs(new_dataset_path)
        csv_per_cam = {cam_dir.name: cam_dir / "data.csv" for cam_dir in filtered_cam_dirs}
        timestamps_per_cam = {
            cam_name: get_timestamps_from_csv(csv_path)
            for cam_name, csv_path in csv_per_cam.items()
        }
        filenames_per_cam = {
            cam_name: get_filenames_from_csv(csv_path)
            for cam_name, csv_path in csv_per_cam.items()
        }

    # Encode one video per camera.
    encoded_any = False

    for cam_name, timestamps in timestamps_per_cam.items():
        output_dir = video_base / "mav0" / cam_name
        output_dir.mkdir(parents=True, exist_ok=True)

        if encoding_options["use_dataset_framerate"]:
            framerate = compute_framerate(csv_per_cam[cam_name].parent)
        else:
            framerate = encoding_options["framerate"]

        concat_txt = output_dir / "input.txt"
        build_ffmpeg_concat_file(filenames_per_cam[cam_name], original_data_dirs[cam_name], concat_txt, framerate)
        print(f"Written concat file: {concat_txt} ({len(timestamps)} entries, {framerate} fps)")

        output_video = output_dir / "data.mp4"
        print(
            f"Encoding {len(timestamps)} frames from {original_data_dirs[cam_name]} "
            f"into {output_video} at {framerate} fps"
        )

        passlogfile = str(output_dir / "ffmpeg2pass")

        # Keep -r before -i so concat input is read at the expected frame rate.
        base_cmd_parts = [
            "ffmpeg",
            "-y",
            "-f", "concat",
            "-safe", "0",
            "-r", str(framerate),
            "-i", str(concat_txt),
            "-c:v", encoding_options["encoder"],
            "-b:v", encoding_options["bitrate"],
            "-pix_fmt", encoding_options["pix_fmt"],
            "-r", str(framerate),
            "-vf", "setpts=PTS-STARTPTS"
        ]

        if encoding_options["x264opts"] is not None and encoding_options["encoder"] == "libx264":
            base_cmd_parts.extend(["-x264opts", encoding_options["x264opts"]])

        pass1_cmd_parts = base_cmd_parts + [
            "-passlogfile", passlogfile,
            "-pass", "1",
            "-an",
            "-f", "null",
            "/dev/null",
        ]
        pass1_cmd = " ".join(shlex.quote(p) for p in pass1_cmd_parts)
        stdout, returncode = shout(pass1_cmd)
        assert returncode == 0, f"ffmpeg pass 1 failed for {cam_name}:\n{stdout}"

        pass2_cmd_parts = base_cmd_parts + [
            "-passlogfile", passlogfile,
            "-pass", "2",
            "-an",
            "-f", "mp4",
            str(output_video),
        ]
        pass2_cmd = " ".join(shlex.quote(p) for p in pass2_cmd_parts)
        stdout, returncode = shout(pass2_cmd)
        assert returncode == 0, f"ffmpeg pass 2 failed for {cam_name}:\n{stdout}"

        print(f"Successfully created {output_video}")
        encoded_any = True

    if not encoded_any:
        print(f"No videos were encoded from dataset {dataset_path}")
        return None, None

    return video_base, new_dataset_path


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
    parser.add_argument(
        "--config_path",
        type=str,
        default=None,
        help="Path to the basalt config JSON file. Used to check if MV video "
        "generation is needed (looks for config.use_video_frames).",
    )
    parser.add_argument(
        "--device",
        type=str,
        default=None,
        help="Device name (e.g., euroc, tumvi, msdmo). Used for video output path under <working_dir>/videos/.",
    )
    return parser.parse_args()



def main():
    """Main function to handle dataset uncompression and optional video generation."""
    args = parse_args()
    dataset_path = args.dataset_path
    working_dir = args.working_dir
    lock_file = Path(args.lock_file)

    zip_path = dataset_path.with_suffix(".zip")

    working_mount_point = get_mount_point(working_dir)
    print(f"{working_mount_point=}")

    final_ds_path = None

    if not zip_path.exists():
        dataset_mount_point = get_mount_point(dataset_path)
        print(f"{dataset_mount_point=}")
        if dataset_mount_point == working_mount_point:
            final_ds_path = dataset_path
        else:
            print("No zip found and mount points differ — cannot proceed")
            sys.exit(1)
    else:
        dataset_mount_point = get_mount_point(zip_path)
        print(f"{dataset_mount_point=}")
        print(f"Uncompressing {zip_path} to {working_dir}")

        # Naive locking (race conditions possible but rare)
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
            final_ds_path = working_dir / dataset_path.name
        except Exception as e:
            print(f"Failed to obtain dataset: {e}")

        # Release lock
        lock_file.unlink(missing_ok=True)

    # optional video generation 
    if args.config_path is not None:
        generate_videos, encoding_options = get_video_processing_config(args.config_path)
        if final_ds_path is not None and generate_videos:
            device = args.device or "unknown"
            print(f"Config requests MV video generation for device '{device}'")
            video_path, new_dataset_path = output_videos(
                working_dir, final_ds_path, device, encoding_options
            )
            if video_path:
                print(f"VIDEO_PATH={video_path}")
                # When a filtered EuRoC was recreated due to a timestamp mismatch,
                # the recreated dataset IS the final dataset path to use downstream.
                if new_dataset_path is not None:
                    final_ds_path = new_dataset_path
            else:
                print("VIDEO_PATH=")

    # Final output line: dataset path (must remain last for backward compatibility)
    print(final_ds_path)


if __name__ == "__main__":
    main()
