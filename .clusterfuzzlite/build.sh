#!/bin/bash -eu
# ClusterFuzzLite build entry point.
#
# The platform copies this file to the repository root and runs it there, with
# $SRC pointing at the repository, $OUT the artifact directory, and $CXX /
# $LIB_FUZZING_ENGINE supplied by the base image. Every fuzz target is compiled
# and written into $OUT; each links the same set of first-party objects so a
# crash always lands in project code under src/, never in a harness.
: "${SRC:?SRC is required}"
: "${OUT:?OUT is required}"
: "${CXX:?CXX is required}"
: "${LIB_FUZZING_ENGINE:?LIB_FUZZING_ENGINE is required}"

CXXFLAGS="${CXXFLAGS:-} -std=c++17 -I$SRC/include -I$SRC/fuzz"

# Every first-party translation unit, discovered by glob so a new subsystem is
# picked up without editing this script. A plain read loop keeps this working
# under both the base image's bash and older shells that lack `mapfile`.
COMMON_SRC=()
while IFS= read -r f; do
  COMMON_SRC+=("$f")
done < <(find "$SRC/src" -name '*.cpp' | sort)
if [ "${#COMMON_SRC[@]}" -eq 0 ]; then
  echo "build.sh: no sources found under $SRC/src" >&2
  exit 1
fi

# The fuzz targets. Each name maps to fuzz/fuzz_<name>.cc and is emitted as the
# executable $OUT/<name>.
TARGETS=(
  decode
  report
  query
  derive
  assemble
  export
  rewrite
  frame
)

OBJDIR="$OUT/obj"
mkdir -p "$OBJDIR"
OBJECTS=()
for src in "${COMMON_SRC[@]}"; do
  rel="${src#"$SRC"/}"           # path relative to the repo root
  obj="$OBJDIR/${rel//\//_}"     # flatten so basenames never collide
  obj="${obj%.cpp}.o"
  "$CXX" $CXXFLAGS -c "$src" -o "$obj"
  OBJECTS+=("$obj")
done

for target in "${TARGETS[@]}"; do
  "$CXX" $CXXFLAGS "${OBJECTS[@]}" "$SRC/fuzz/fuzz_${target}.cc" \
    $LIB_FUZZING_ENGINE -o "$OUT/${target}"
done

# Package each target's seed corpus as $OUT/<target>_seed_corpus.zip, which the
# platform unpacks as that target's starting inputs. python3 is used because the
# shell `zip` binary is not guaranteed to be present in the base image.
python3 - "$SRC/fuzz/corpus" "$OUT" <<'PY'
import os, sys, zipfile
corpus_root, out = sys.argv[1], sys.argv[2]
if os.path.isdir(corpus_root):
    for target in sorted(os.listdir(corpus_root)):
        d = os.path.join(corpus_root, target)
        if not os.path.isdir(d):
            continue
        with zipfile.ZipFile(os.path.join(out, target + "_seed_corpus.zip"),
                             "w", zipfile.ZIP_DEFLATED) as z:
            for name in sorted(os.listdir(d)):
                fp = os.path.join(d, name)
                if os.path.isfile(fp):
                    z.write(fp, arcname=name)
PY
