#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

exec tools/bazel run \
    --config=stamp \
    --config=gs101 \
    //private/devices/google/gs101:dist "$@"
