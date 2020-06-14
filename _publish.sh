#! /usr/bin/env bash
set -euo pipefail
set -x

cobalt build

build_dir="$(pwd)/_site"
tmp_dir=$(mktemp -d)
checkout_dir="${tmp_dir}/checkout"

pushd "${build_dir}"
tar -caf site.tar *
popd

git worktree add "${checkout_dir}" master
pushd "${checkout_dir}"
mv "${build_dir}/site.tar" .
tar -xaf "site.tar"
git add .
git commit -m "Updated website build on $(date)"
git push
popd

#git worktree remove ${checkout_dir}
