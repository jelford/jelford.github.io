#! /usr/bin/env bash
set -euo pipefail

cobalt build

build_dir="$(pwd)/_site"
tmp_dir=$(mktemp -d)
checkout_dir="${tmp_dir}/checkout"

git worktree add "${checkout_dir}" publish
pushd "${checkout_dir}"
rm -r ./*
cp -r "${build_dir}/*" .
git add .
git commit -m "Updated website build on $(date)"
git push
popd

git worktree remove ${checkout_dir}
