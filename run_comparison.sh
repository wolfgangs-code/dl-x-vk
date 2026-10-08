#!/usr/bin/env bash
set -e

# Ask for sudo once upfront so no polkit popups occur during the test
sudo python3 "$(dirname "$0")/scripts/compare_benchmark.py" "$@"
