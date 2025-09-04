#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run \
    --config=stamp \
    --config=bluejay \
    //private/devices/google/bluejay:gs101_bluejay_dist "$@"
