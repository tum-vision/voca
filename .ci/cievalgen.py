#!/usr/bin/env python3

# Copyright 2026, Mateo de Mayo
# SPDX-License-Identifier: BSD-3-Clause
# Author: Mateo de Mayo <mateo.demayo@tum.de>

"""Generate CI evaluation YAML file for GitLab CI/CD."""

import argparse
import json
import copy
from textwrap import dedent

EVALSET_TEMPLATE = dedent(
    """
    {evalset}:
        stage: {stage}
        tags: [{tags}]
        extends: .run-dataset
        variables:
          DETERMINISTIC: {deterministic}
          NUM_THREADS: {num_threads}
          REPETITIONS: {repetitions}
          RESULTS_DIR: {results_dir}
        parallel:
          matrix:
            - DATASET: [{datasets}]
    """
)

TIMING_EVALSET_TEMPLATE = dedent(
    """
    {evalset}:
        stage: {stage}
        tags: [{tags}]
        extends: .run-dataset
        resource_group: run_alone
        variables:
          DETERMINISTIC: {deterministic}
          NUM_THREADS: {num_threads}
          REPETITIONS: {repetitions}
          RESULTS_DIR: {results_dir}
        parallel:
          matrix:
            - DATASET: [{datasets}]
    """
)


def parse_args():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "description", type=str, help="Path to the evaluation set description JSON file"
    )
    parser.add_argument(
        "evalsets",
        type=str,
        help="Comma-separated list (without spaces) of selected evalsets to"
        "generate CI file from, e.g., 'euroc-v1,quickset1,EMH05'",
    )
    parser.add_argument(
        "timing_evalsets",
        type=str,
        help="Same as evalsets but for timing evaluation.",
    )
    parser.add_argument(
        "template",
        type=str,
        help="Path to the CI YAML template to fill in",
    )
    parser.add_argument(
        "output",
        type=str,
        help="Path to output the filled in YAML file",
    )
    parser.add_argument(
        "--deterministic",
        type=int,
        default=1,
        help="Set to 1 by default for deterministic evaluation. ",
    )
    parser.add_argument(
        "--num_threads",
        type=int,
        default=0,
        help="Number of threads to use for evaluation runs. 0 means auto-detect.",
    )
    parser.add_argument(
        "--timing_deterministic",
        type=int,
        default=1,
        help="Set to 1 by default for deterministic timing evaluation.",
    )
    parser.add_argument(
        "--timing_repetitions",
        type=int,
        default=1,
        help="Runs can be performed multiple times for warming up the system.",
    )
    parser.add_argument(
        "--timing_num_threads",
        type=int,
        default=0,
        help="Number of threads to use for timing evaluation runs. 0 means auto-detect.",
    )

    return parser.parse_args()


def add_custom_evalset(evalsets, evaluations):
    custom_evalset = []
    for i, string in enumerate(list(evalsets)):
        if string in evaluations["evalsets"]:
            continue

        if string in evaluations["sequences"]:
            custom_evalset.append(string)
        else:
            print(f"Ignoring invalid evalset/sequence: {string}")

        evalsets.remove(string)
    if len(custom_evalset) > 0:
        evaluations["evalsets"]["custom"] = custom_evalset
        evalsets.append("custom")


def main():
    "Main function"
    args = parse_args()
    description = args.description
    evalsets = args.evalsets.split(",")
    timing_evalsets = args.timing_evalsets.split(",")
    template = args.template
    output = args.output
    deterministic = args.deterministic
    num_threads = args.num_threads
    timing_deterministic = args.timing_deterministic
    timing_repetitions = args.timing_repetitions
    timing_num_threads = args.timing_num_threads

    tcontents = open(template, "r", encoding="utf-8").read()
    evaluations = json.load(open(description, "r", encoding="utf-8"))
    timing_evaluations = copy.deepcopy(evaluations)

    jobs = []
    tjobs = []

    # Optionally add a `custom` evalset if individual sequences were specified
    add_custom_evalset(evalsets, evaluations)
    add_custom_evalset(timing_evalsets, timing_evaluations)

    for evalset in evalsets:
        datasets = ", ".join(evaluations["evalsets"][evalset])
        job = EVALSET_TEMPLATE.format(
            stage="evalsets",
            evalset=evalset,
            datasets=datasets,
            deterministic=deterministic,
            num_threads=num_threads,
            repetitions=1,
            tags="basalt-evaluation-box",
            results_dir="results",
        )
        jobs.append(job)
    jobs_str = f"".join(jobs).strip()

    for evalset in timing_evalsets:
        datasets = ", ".join(timing_evaluations["evalsets"][evalset])
        job = TIMING_EVALSET_TEMPLATE.format(
            stage="timing",
            evalset=f"timing:{evalset}",
            datasets=datasets,
            deterministic=timing_deterministic,
            num_threads=timing_num_threads,
            repetitions=timing_repetitions,
            tags="basalt-evaluation-box",
            results_dir="timing-results",
        )
        tjobs.append(job)
    tjobs_str = f"".join(tjobs).strip()

    contents = tcontents.format(
        evalset_list=args.evalsets,
        evalsets_jobs=jobs_str,
        deterministic=deterministic,
        num_threads=num_threads,
        timing_evalset_list=args.timing_evalsets,
        timing_jobs=tjobs_str,
        timing_deterministic=timing_deterministic,
        timing_num_threads=timing_num_threads,
        timing_repetitions=timing_repetitions,
    )
    open(output, "w", encoding="utf-8").write(contents)


if __name__ == "__main__":
    main()
