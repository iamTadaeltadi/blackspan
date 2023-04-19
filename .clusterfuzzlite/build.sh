#!/bin/bash -eu
: "${SRC:?SRC is required}"
: "${OUT:?OUT is required}"
: "${CXX:?CXX is required}"
: "${LIB_FUZZING_ENGINE:?LIB_FUZZING_ENGINE is required}"

CXXFLAGS="${CXXFLAGS:-} -std=c++17 -I$SRC/include -I$SRC/fuzz"

COMMON_SRC=()
while IFS= read -r f; do
  COMMON_SRC+=("$f")
done < <(find "$SRC/src" -name '*.cpp' | sort)
if [ "${#COMMON_SRC[@]}" -eq 0 ]; then
  echo "build.sh: no sources found under $SRC/src" >&2
  exit 1
fi

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
  rel="${src#"$SRC"/}"
  obj="$OBJDIR/${rel//\//_}"
  obj="${obj%.cpp}.o"
  "$CXX" $CXXFLAGS -c "$src" -o "$obj"
  OBJECTS+=("$obj")
done

for target in "${TARGETS[@]}"; do
  "$CXX" $CXXFLAGS "${OBJECTS[@]}" "$SRC/fuzz/fuzz_${target}.cc" \
    $LIB_FUZZING_ENGINE -o "$OUT/${target}"
done

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
