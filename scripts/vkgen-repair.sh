#!/bin/sh
# Regenerate every Vulkan shader source serially and replace any
# *.comp.cpp that a parallel build left incomplete.
#
# The build-time generator (vulkan-shaders-gen) spawns up to
# min(16, nproc) glslc subprocesses at once and SILENTLY drops any kernel
# whose subprocess fails to fork (fork() -> -1 / ENOMEM / EAGAIN under
# memory pressure), while still declaring the kernel in
# ggml-vulkan-shaders.hpp.
#
# Returns 0 on success (whether or not any file needed repairing), and a
# non-zero status only on a hard failure.
set -eu

build=${1:?usage: vkgen-repair.sh BUILD_DIR GLSLC}
glslc=${2:?usage:  vkgen-repair.sh BUILD_DIR GLSLC}

llamacpp=$(cd "$(dirname "$build")" && pwd)
build=$(cd "$build" && pwd)

gen="$build/Release/vulkan-shaders-gen"
src_dir="$llamacpp/ggml/src/ggml-vulkan/vulkan-shaders"
bin_dir="$build/ggml/src/ggml-vulkan"
hdr="$bin_dir/ggml-vulkan-shaders.hpp"
out_dir="$bin_dir/vulkan-shaders.spv"
tmp="$bin_dir/.vkgen-repair.cpp"
err="$bin_dir/.vkgen-repair.err"
wrapper="$bin_dir/.glslc-cached.sh"

[ -x "$gen" ] || exit 0

cat > "$wrapper" <<EOF
#!/bin/sh
prev=""
for arg in "\$@"; do
    if [ "\$prev" = "-o" ] && [ -s "\$arg" ]; then
        exit 0
    fi
    prev="\$arg"
done
exec "$glslc" "\$@"
EOF
chmod +x "$wrapper"

dirty=0
for src in "$src_dir"/*.comp; do
    name=$(basename "$src")
    target="$bin_dir/${name}.cpp"

    ok=0
    for attempt in 1 2 3 4 5; do
        rm -f "$tmp" "$err"
        "$gen" --glslc "$wrapper" --source "$src" --output-dir "$out_dir" \
               --target-hpp "$hdr" --target-cpp "$tmp" > /dev/null 2> "$err" || true
        if [ -s "$tmp" ] && ! grep -qE 'cannot compile|Error executing' "$err"; then
            ok=1
            break
        fi
        echo "  retry $attempt/5: dropping kernels in $name"
        sleep 2
    done

    if [ "$ok" -ne 1 ]; then
        echo "ERROR: could not generate all kernels for $name" >&2
        cat "$err" >&2
        rm -f "$tmp" "$err" "$wrapper"
        exit 1
    fi

    if ! cmp -s "$tmp" "$target"; then
        mv -f "$tmp" "$target"
        echo "repaired $name"
        dirty=1
    else
        rm -f "$tmp"
    fi
    rm -f "$err"
done

rm -f "$wrapper"
exit 0
