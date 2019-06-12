set -e
bazel build //src:main
./bazel-bin/src/main
