#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Creates the parity environment: a Python 3.12 venv with pinned NumPy 1.x, and ESA tetra3 at
# the reference commit (plus a copy with a stable magnitude sort for generator parity).
#   tests/parity/setup_env.sh <dir>
set -euo pipefail
dir=${1:?target dir}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$dir"
py=${PYTHON:-python3.12}
[ -x "$dir/venv/bin/python" ] || "$py" -m venv "$dir/venv"
"$dir/venv/bin/pip" install -q -r "$here/requirements.txt"
if [ ! -d "$dir/tetra3" ]; then
  git clone -q https://github.com/esa/tetra3 "$dir/tetra3"
fi
git -C "$dir/tetra3" checkout -q f9fa2eb
mkdir -p "$dir/tetra3_stable/tetra3"
cp "$dir/tetra3/tetra3/__init__.py" "$dir/tetra3_stable/tetra3/"
sed -e "s/brightness_ii = np.argsort(star_table\[:, -1\])/brightness_ii = np.argsort(star_table[:, -1], kind='stable')/" \
  "$dir/tetra3/tetra3/tetra3.py" > "$dir/tetra3_stable/tetra3/tetra3.py"
grep -q "kind='stable'" "$dir/tetra3_stable/tetra3/tetra3.py"
echo "parity env ready in $dir"
