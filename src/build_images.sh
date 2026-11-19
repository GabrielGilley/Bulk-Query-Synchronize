#!/bin/bash -ex

P="${P:-podman}"        # Run with P=docker ./build_images.sh to change

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${SOURCE_DIR}/$(basename "${BASH_SOURCE[0]}")"

# Build Docker archives and SIFs on local disk, not /ascldap.
# Override if needed:
#   BQS_LOCAL_BUILD_ROOT=/var/tmp/$USER/bqs-build P=docker ./build_images.sh
LOCAL_BUILD_ROOT="${BQS_LOCAL_BUILD_ROOT:-/tmp/${USER}/bqs-build}"
APPTAINER_WORK_ROOT="${LOCAL_BUILD_ROOT}/apptainer"
SIF_WORK_ROOT="${LOCAL_BUILD_ROOT}/sifs"
TAR_WORK_ROOT="${LOCAL_BUILD_ROOT}/tars"
LOG_DIR="${SOURCE_DIR}/sifs/logs"

mkdir -p \
    "${APPTAINER_WORK_ROOT}" \
    "${SIF_WORK_ROOT}" \
    "${TAR_WORK_ROOT}" \
    "${LOG_DIR}"

# Some Apptainer/Singularity versions support this. It helps avoid
# mksquashfs segfaults/races on this system.
SIF_BUILD_OPTS=()
if singularity build --help 2>&1 | grep -q -- '--mksquashfs-args'; then
    SIF_BUILD_OPTS+=(--mksquashfs-args "-processors 1")
fi

# .sif conversions run in the background, at most $JOBS at a time. With
# mksquashfs held to one processor each, more of them can run at once.
if (( ${#SIF_BUILD_OPTS[@]} )); then
    JOBS="${JOBS:-8}"
else
    JOBS="${JOBS:-4}"
fi

# Convert one image to sifs/<name>.sif. Each conversion has its own
# Apptainer tmp and cache directories, since a retry wipes its tmp
# directory and must not touch another conversion's files.
function convert_sif() {
    local name="$1"

    local tar_file="${TAR_WORK_ROOT}/${name}.tar"
    local sif_tmp="${SIF_WORK_ROOT}/${name}.sif"
    local sif_final="${SOURCE_DIR}/sifs/${name}.sif"
    local work_tmp="${APPTAINER_WORK_ROOT}/tmp/${name}"
    local work_cache="${APPTAINER_WORK_ROOT}/cache/${name}"

    export TMPDIR="${work_tmp}"
    export APPTAINER_TMPDIR="${work_tmp}"
    export APPTAINER_CACHEDIR="${work_cache}"
    export SINGULARITY_TMPDIR="${work_tmp}"
    export SINGULARITY_CACHEDIR="${work_cache}"
    mkdir -p "${work_tmp}" "${work_cache}"

    rm -vf "${tar_file}" "${sif_tmp}"

    "$P" image save -o "${tar_file}" "${name}"

    local ok=0
    local attempt

    for attempt in 1 2 3; do
        echo "Attempt ${attempt}/3: building ${name}.sif"
        rm -vf "${sif_tmp}"
        rm -rf "${work_tmp:?}/"*

        if singularity build "${SIF_BUILD_OPTS[@]}" "${sif_tmp}" "docker-archive://${tar_file}"; then
            ok=1
            break
        fi

        echo "WARNING: singularity build failed for ${name} on attempt ${attempt}"
        sleep 5
    done

    if [ "${ok}" -ne 1 ]; then
        echo "ERROR: failed to build SIF for ${name} after 3 attempts"
        exit 1
    fi

    # Copy next to the final name, then rename over it: a rename gives the
    # new .sif its own file, where copying onto the old one rewrote it in
    # place under any container still running from it.
    cp -vf "${sif_tmp}" "${sif_final}.tmp"
    mv -vf "${sif_final}.tmp" "${sif_final}"

    rm -vf "${tar_file}" "${sif_tmp}"
    rm -rf "${work_tmp}"
}

# The script runs itself to do one conversion (see sif_bg)
if [[ "${1:-}" == "--convert-sif" ]]; then
    convert_sif "$2"
    exit 0
fi

pushd "${SOURCE_DIR}"

SIF_PIDS=()
SIF_NAMES=()

function cleanup() {
    # Stop conversions still running if we bail out early. Each runs in its
    # own session (setsid), so killing its process group also stops the
    # podman, singularity and mksquashfs it started.
    for pid in "${SIF_PIDS[@]}"; do
        kill -- "-${pid}" 2>/dev/null || kill "${pid}" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT
# Ctrl-C or a kill: exit, so the EXIT trap stops the conversions
trap 'exit 130' INT TERM

function build_im() {
    "$P" image build -t "$1" -f "env/$1" "$2"
}

# Start a .sif conversion in the background, throttled to $JOBS at a time.
function sif_bg() {
    # Poll rather than `wait -n`, which would consume exit statuses checked
    # at the end.
    { set +x; } 2>/dev/null
    while (( $(jobs -rp | wc -l) >= JOBS )); do sleep 1; done
    set -x
    setsid "${SCRIPT}" --convert-sif "$1" > "${LOG_DIR}/$1.log" 2>&1 &
    SIF_PIDS+=($!)
    SIF_NAMES+=("$1")
}

function build_sif() {
    build_im "$@"
    sif_bg "$1"
}

# Image builds stay sequential (each depends on the previous one); .sif
# conversions start in the background as soon as their image exists.
build_im pando_base_os env
build_sif pando_dev env         # converts while pando_build compiles
build_im pando_build .
build_im pando_run env
build_sif pando_proxy env
build_sif pando_pardb env
build_sif pando_client env
build_sif pando_mon env
build_sif pando_top env
build_sif pando_python env
build_sif pando_jupyter env

# Check every conversion and report failures with their logs.
set +x
FAILED=()
for i in "${!SIF_PIDS[@]}"; do
    if ! wait "${SIF_PIDS[$i]}"; then
        FAILED+=("${SIF_NAMES[$i]}")
        echo "===== ${SIF_NAMES[$i]} FAILED (log: ${LOG_DIR}/${SIF_NAMES[$i]}.log) ====="
        tail -n 30 "${LOG_DIR}/${SIF_NAMES[$i]}.log"
    fi
done
SIF_PIDS=()
if (( ${#FAILED[@]} )); then
    echo "sif conversion failed for: ${FAILED[*]}"
    exit 1
fi
echo "All sifs built; logs in ${LOG_DIR}"

popd
