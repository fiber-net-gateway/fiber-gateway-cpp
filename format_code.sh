#!/bin/bash

set -e

git ls-files '*.cpp' '*.h'|xargs clang-format -i
