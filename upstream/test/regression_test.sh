#!/usr/bin/env bash
#
# Headless regression test for virtualjaguar-libretro
#
# Runs test ROMs via miniretro, compares screenshots against reference
# images, and generates visual diffs on failure.
#
# Usage: ./test/regression_test.sh <core_path>
# Example: ./test/regression_test.sh ./virtualjaguar_libretro.so
#
# Set MINIRETRO_BIN env var to skip building miniretro from source.
# Set DIFF_DIR env var to specify where diff images are saved.
#
set -euo pipefail

CORE="${1:?Usage: $0 <core_path>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK_DIR="$(mktemp -d)"
BASELINE_DIR="${SCRIPT_DIR}/baselines"
ROM_DIR="${SCRIPT_DIR}/roms"
DIFF_DIR="${DIFF_DIR:-${WORK_DIR}/diffs}"
# 600 frames (~10 seconds at 60fps) to get past BIOS boot
FRAMES=600
DUMP_EVERY=100

trap 'rm -rf "${WORK_DIR}"' EXIT

# --- Detect platform for build flags ---
LDFLAGS="-ldl"
BUILD_CXX="${CXX:-g++}"
case "$(uname -s)" in
    Darwin)
        LDFLAGS=""
        BUILD_CXX="${CXX:-clang++}"
        ;;
esac

# --- Get miniretro (use env var if pre-built, otherwise build from source) ---
if [ -n "${MINIRETRO_BIN:-}" ] && [ -x "${MINIRETRO_BIN}" ]; then
    echo "==> Using pre-built miniretro: ${MINIRETRO_BIN}"
else
    MINIRETRO_BIN="${WORK_DIR}/miniretro-bin"
    echo "==> Building miniretro..."
    git clone --depth 1 https://github.com/davidgfnet/miniretro.git "${WORK_DIR}/miniretro" 2>/dev/null
    ${BUILD_CXX} -O2 -Wall -Wno-deprecated-declarations -Wno-unused-result \
        -o "${MINIRETRO_BIN}" \
        "${WORK_DIR}/miniretro/miniretro.cc" \
        "${WORK_DIR}/miniretro/util.cc" \
        "${WORK_DIR}/miniretro/loader.cc" \
        ${LDFLAGS}

    if [ ! -x "${MINIRETRO_BIN}" ]; then
        echo "ERROR: Failed to build miniretro"
        exit 1
    fi
    echo "==> miniretro built successfully"
fi

echo "==> Baselines: ${BASELINE_DIR}"
echo "==> Diff output: ${DIFF_DIR}"
mkdir -p "${DIFF_DIR}"

# --- Resolve core to absolute path ---
CORE="$(cd "$(dirname "${CORE}")" && pwd)/$(basename "${CORE}")"

# --- Run tests ---
PASS=0
FAIL=0
NEW=0
SUMMARY=""

for rom in "${ROM_DIR}"/*.j64 "${ROM_DIR}"/*.rom; do
    [ -f "${rom}" ] || continue
    rom_name="$(basename "${rom}" | sed 's/\.[^.]*$//')"
    out_dir="${WORK_DIR}/output/${rom_name}"
    mkdir -p "${out_dir}"

    echo "==> Testing: ${rom_name} (${FRAMES} frames)"

    # Run the core headless. --no-alarm disables per-frame timeout
    # since some Jaguar frames are slow (boot, complex rendering).
    "${MINIRETRO_BIN}" \
        --core "${CORE}" --rom "${rom}" \
        --output "${out_dir}" --system "${out_dir}" \
        --frames "${FRAMES}" --dump-frames-every "${DUMP_EVERY}" \
        --no-alarm 2>&1 | head -20 || true

    # Use the last dumped screenshot for comparison
    frame_file=$(find "${out_dir}" -name "screenshot*.png" 2>/dev/null | sort | tail -1)

    if [ -z "${frame_file}" ]; then
        echo "   WARNING: No frame dumped for ${rom_name}"
        FAIL=$((FAIL + 1))
        SUMMARY="${SUMMARY}| ${rom_name} | :x: FAIL | No frame output | - |\n"
        continue
    fi

    echo "   Using frame: $(basename "${frame_file}")"

    # Copy current screenshot to diff dir for reference
    cp "${frame_file}" "${DIFF_DIR}/${rom_name}_current.png"

    baseline_png="${BASELINE_DIR}/${rom_name}.png"

    if [ -f "${baseline_png}" ]; then
        # Compare against reference screenshot
        if command -v compare &>/dev/null; then
            # ImageMagick compare: generate diff image and get metric
            set +e
            metric_raw=$(compare -metric AE "${baseline_png}" "${frame_file}" \
                "${DIFF_DIR}/${rom_name}_diff.png" 2>&1)
            compare_status=$?
            set -e

            if [ "${compare_status}" -le 1 ]; then
                # Extract just the integer pixel count (ImageMagick may output "0 (0)")
                metric=$(printf '%s\n' "${metric_raw}" | awk 'NR==1 {print $1}')

                if [[ "${metric}" =~ ^[0-9]+$ ]] && [ "${metric}" = "0" ]; then
                    echo "   PASS: ${rom_name} (0 pixels differ)"
                    PASS=$((PASS + 1))
                    SUMMARY="${SUMMARY}| ${rom_name} | :white_check_mark: PASS | 0 pixels differ | - |\n"
                    # Clean up diff artifacts on pass
                    rm -f "${DIFF_DIR}/${rom_name}_diff.png" "${DIFF_DIR}/${rom_name}_current.png"
                elif [[ "${metric}" =~ ^[0-9]+$ ]]; then
                    echo "   FAIL: ${rom_name} (${metric} pixels differ)"
                    # Also generate a side-by-side comparison
                    if command -v montage &>/dev/null; then
                        montage "${baseline_png}" "${frame_file}" "${DIFF_DIR}/${rom_name}_diff.png" \
                            -tile 3x1 -geometry +4+4 -label '%f' \
                            "${DIFF_DIR}/${rom_name}_sidebyside.png" 2>/dev/null || true
                    fi
                    cp "${baseline_png}" "${DIFF_DIR}/${rom_name}_expected.png"
                    FAIL=$((FAIL + 1))
                    SUMMARY="${SUMMARY}| ${rom_name} | :x: FAIL | ${metric} pixels differ | See artifacts |\n"
                else
                    echo "   FAIL: ${rom_name} (compare error: ${metric_raw})"
                    cp "${baseline_png}" "${DIFF_DIR}/${rom_name}_expected.png"
                    FAIL=$((FAIL + 1))
                    SUMMARY="${SUMMARY}| ${rom_name} | :x: FAIL | compare error | See artifacts |\n"
                fi
            else
                echo "   FAIL: ${rom_name} (compare failed: ${metric_raw})"
                cp "${baseline_png}" "${DIFF_DIR}/${rom_name}_expected.png"
                FAIL=$((FAIL + 1))
                SUMMARY="${SUMMARY}| ${rom_name} | :x: FAIL | compare error | See artifacts |\n"
            fi
        else
            # Fallback: byte-level comparison
            if cmp -s "${baseline_png}" "${frame_file}"; then
                echo "   PASS: ${rom_name} (identical)"
                PASS=$((PASS + 1))
                SUMMARY="${SUMMARY}| ${rom_name} | :white_check_mark: PASS | identical | - |\n"
            else
                echo "   FAIL: ${rom_name} (screenshots differ)"
                cp "${baseline_png}" "${DIFF_DIR}/${rom_name}_expected.png"
                FAIL=$((FAIL + 1))
                SUMMARY="${SUMMARY}| ${rom_name} | :x: FAIL | screenshots differ | See artifacts |\n"
            fi
        fi
    else
        echo "   NEW: ${rom_name} — no baseline yet"
        echo "   To create: cp \"${frame_file}\" \"${baseline_png}\""
        NEW=$((NEW + 1))
        SUMMARY="${SUMMARY}| ${rom_name} | :new: NEW | no baseline | - |\n"
    fi
done

# --- Helper: run ROM and return last screenshot path ---
run_and_get_frame() {
    local out_dir="$1" envvar_args="$2"
    shift 2
    # shellcheck disable=SC2086
    "${MINIRETRO_BIN}" \
        --core "${CORE}" --rom "$1" \
        --output "${out_dir}" --system "${out_dir}" \
        --frames "${FRAMES}" --dump-frames-every "${DUMP_EVERY}" \
        --no-alarm ${envvar_args} >/dev/null 2>&1 || true
    find "${out_dir}" -name "screenshot*.png" 2>/dev/null | sort | tail -1
}

# --- Determinism test: run each ROM twice, verify identical output ---
# Validates that emulation is fully deterministic (no rand() in hot paths).
echo ""
echo "==> Running determinism check..."
for rom in "${ROM_DIR}"/*.j64 "${ROM_DIR}"/*.rom; do
    [ -f "${rom}" ] || continue
    rom_name="$(basename "${rom}" | sed 's/\.[^.]*$//')"
    det_dir1="${WORK_DIR}/det1/${rom_name}"
    det_dir2="${WORK_DIR}/det2/${rom_name}"
    mkdir -p "${det_dir1}" "${det_dir2}"

    frame1=$(run_and_get_frame "${det_dir1}" "" "${rom}")
    frame2=$(run_and_get_frame "${det_dir2}" "" "${rom}")

    if [ -n "${frame1}" ] && [ -n "${frame2}" ]; then
        if cmp -s "${frame1}" "${frame2}"; then
            echo "   PASS: ${rom_name} determinism (identical across runs)"
            PASS=$((PASS + 1))
            SUMMARY="${SUMMARY}| ${rom_name} (determinism) | :white_check_mark: PASS | identical across runs | - |\n"
        else
            echo "   FAIL: ${rom_name} determinism (output differs between runs)"
            cp "${frame1}" "${DIFF_DIR}/${rom_name}_det_run1.png"
            cp "${frame2}" "${DIFF_DIR}/${rom_name}_det_run2.png"
            FAIL=$((FAIL + 1))
            SUMMARY="${SUMMARY}| ${rom_name} (determinism) | :x: FAIL | non-deterministic output | See artifacts |\n"
        fi
    else
        echo "   FAIL: ${rom_name} determinism (no frames produced)"
        FAIL=$((FAIL + 1))
        SUMMARY="${SUMMARY}| ${rom_name} (determinism) | :x: FAIL | no frames produced | - |\n"
    fi
done

# --- Frameskip test: verify core options don't affect emulation output ---
# With frameskip, video_cb receives NULL on skipped frames but the
# emulation still runs identically. The last rendered frame should match.
# NOTE: requires the virtualjaguar_frameskip core option (PR #96).
# Without it, both runs use default config, so the test trivially passes.
echo ""
echo "==> Running frameskip invariance check..."
for rom in "${ROM_DIR}"/*.j64 "${ROM_DIR}"/*.rom; do
    [ -f "${rom}" ] || continue
    rom_name="$(basename "${rom}" | sed 's/\.[^.]*$//')"
    fs0_dir="${WORK_DIR}/fs0/${rom_name}"
    fs3_dir="${WORK_DIR}/fs3/${rom_name}"
    mkdir -p "${fs0_dir}" "${fs3_dir}"

    frame_fs0=$(run_and_get_frame "${fs0_dir}" "" "${rom}")
    frame_fs3=$(run_and_get_frame "${fs3_dir}" "--envvar virtualjaguar_frameskip=3" "${rom}")

    if [ -n "${frame_fs0}" ] && [ -n "${frame_fs3}" ]; then
        if cmp -s "${frame_fs0}" "${frame_fs3}"; then
            echo "   PASS: ${rom_name} frameskip invariance (skip=0 matches skip=3)"
            PASS=$((PASS + 1))
            SUMMARY="${SUMMARY}| ${rom_name} (frameskip) | :white_check_mark: PASS | skip=0 matches skip=3 | - |\n"
        else
            echo "   FAIL: ${rom_name} frameskip invariance (output differs with frameskip)"
            cp "${frame_fs0}" "${DIFF_DIR}/${rom_name}_fs0.png"
            cp "${frame_fs3}" "${DIFF_DIR}/${rom_name}_fs3.png"
            FAIL=$((FAIL + 1))
            SUMMARY="${SUMMARY}| ${rom_name} (frameskip) | :x: FAIL | frameskip changes output | See artifacts |\n"
        fi
    else
        echo "   FAIL: ${rom_name} frameskip (no frames produced)"
        FAIL=$((FAIL + 1))
        SUMMARY="${SUMMARY}| ${rom_name} (frameskip) | :x: FAIL | no frames produced | - |\n"
    fi
done

# --- Save state round-trip test ---
# Validates retro_serialize/retro_unserialize: save state at frame N,
# load it in a fresh run, continue for M frames.  Compare the final
# screenshot with frame N+M of a straight reference run.
#
# miniretro timing: --dump-savestates-every N captures state AFTER
# retro_run for frame N.  Loading that state and running M more
# retro_runs produces the same output as a straight run's frame N+M.
echo ""
echo "==> Running save state round-trip test..."
SS_SAVE_AT=200
SS_RESUME_LEN=200
SS_REF_FRAME=$((SS_SAVE_AT + SS_RESUME_LEN))
for rom in "${ROM_DIR}"/*.j64 "${ROM_DIR}"/*.rom; do
    [ -f "${rom}" ] || continue
    rom_name="$(basename "${rom}" | sed 's/\.[^.]*$//')"

    # Run 1: straight reference run, capture screenshot at target frame
    ss_ref="${WORK_DIR}/ss_ref/${rom_name}"
    mkdir -p "${ss_ref}"
    "${MINIRETRO_BIN}" \
        --core "${CORE}" --rom "${rom}" \
        --output "${ss_ref}" --system "${ss_ref}" \
        --frames $((SS_REF_FRAME + 1)) \
        --dump-frames-every "${SS_REF_FRAME}" \
        --no-alarm >/dev/null 2>&1 || true
    ref_frame=$(find "${ss_ref}" -name "screenshot*.png" 2>/dev/null | sort | tail -1)

    # Run 2: save state at frame SS_SAVE_AT
    ss_save="${WORK_DIR}/ss_save/${rom_name}"
    mkdir -p "${ss_save}"
    "${MINIRETRO_BIN}" \
        --core "${CORE}" --rom "${rom}" \
        --output "${ss_save}" --system "${ss_save}" \
        --frames $((SS_SAVE_AT + 1)) \
        --dump-savestates-every "${SS_SAVE_AT}" \
        --no-alarm >/dev/null 2>&1 || true
    state_file=$(find "${ss_save}" -name "state*.bin" 2>/dev/null | sort | tail -1)

    # Run 3: load state, run SS_RESUME_LEN frames, capture final screenshot
    # After loading state@N and running M-1 retro_runs, the last
    # screenshot dumped at interval M-1 corresponds to ref frame N+M.
    ss_load="${WORK_DIR}/ss_load/${rom_name}"
    mkdir -p "${ss_load}"
    if [ -n "${state_file}" ]; then
        "${MINIRETRO_BIN}" \
            --core "${CORE}" --rom "${rom}" \
            --output "${ss_load}" --system "${ss_load}" \
            --frames "${SS_RESUME_LEN}" \
            --dump-frames-every $((SS_RESUME_LEN - 1)) \
            --load-savestate "${state_file}" \
            --no-alarm >/dev/null 2>&1 || true
    fi
    load_frame=$(find "${ss_load}" -name "screenshot*.png" 2>/dev/null | sort | tail -1)

    if [ -z "${ref_frame}" ] || [ -z "${state_file}" ] || [ -z "${load_frame}" ]; then
        echo "   FAIL: ${rom_name} save state (missing frames or state file)"
        FAIL=$((FAIL + 1))
        SUMMARY="${SUMMARY}| ${rom_name} (save state) | :x: FAIL | missing output | - |\n"
    elif cmp -s "${ref_frame}" "${load_frame}"; then
        echo "   PASS: ${rom_name} save state round-trip (frame ${SS_REF_FRAME} matches)"
        PASS=$((PASS + 1))
        SUMMARY="${SUMMARY}| ${rom_name} (save state) | :white_check_mark: PASS | round-trip matches | - |\n"
    else
        echo "   FAIL: ${rom_name} save state round-trip (frame ${SS_REF_FRAME} differs)"
        cp "${ref_frame}" "${DIFF_DIR}/${rom_name}_ss_ref.png"
        cp "${load_frame}" "${DIFF_DIR}/${rom_name}_ss_load.png"
        FAIL=$((FAIL + 1))
        SUMMARY="${SUMMARY}| ${rom_name} (save state) | :x: FAIL | round-trip mismatch | See artifacts |\n"
    fi
done

# --- Rewind simulation test ---
# Simulates rewind: run past frame N while saving states periodically,
# then load the frame-N state and run forward again.  The result must
# match the reference from the save state test above.
echo ""
echo "==> Running rewind simulation test..."
RW_REWIND_TO=${SS_SAVE_AT}
RW_REMAIN=${SS_RESUME_LEN}
RW_REF_FRAME=${SS_REF_FRAME}
for rom in "${ROM_DIR}"/*.j64 "${ROM_DIR}"/*.rom; do
    [ -f "${rom}" ] || continue
    rom_name="$(basename "${rom}" | sed 's/\.[^.]*$//')"

    # Reuse reference frame from save state test
    ref_frame=$(find "${WORK_DIR}/ss_ref/${rom_name}" -name "screenshot*.png" 2>/dev/null | sort | tail -1)

    # Run past the rewind point, dumping states every 100 frames
    rw_full="${WORK_DIR}/rw_full/${rom_name}"
    mkdir -p "${rw_full}"
    "${MINIRETRO_BIN}" \
        --core "${CORE}" --rom "${rom}" \
        --output "${rw_full}" --system "${rw_full}" \
        --frames $((RW_REF_FRAME + 1)) \
        --dump-savestates-every 100 \
        --no-alarm >/dev/null 2>&1 || true

    # Find state file for the rewind point
    rw_state="${rw_full}/state$(printf '%06d' ${RW_REWIND_TO}).bin"

    # Load rewind state and continue
    rw_resume="${WORK_DIR}/rw_resume/${rom_name}"
    mkdir -p "${rw_resume}"
    if [ -f "${rw_state}" ]; then
        "${MINIRETRO_BIN}" \
            --core "${CORE}" --rom "${rom}" \
            --output "${rw_resume}" --system "${rw_resume}" \
            --frames "${RW_REMAIN}" \
            --dump-frames-every $((RW_REMAIN - 1)) \
            --load-savestate "${rw_state}" \
            --no-alarm >/dev/null 2>&1 || true
    fi
    rw_frame=$(find "${rw_resume}" -name "screenshot*.png" 2>/dev/null | sort | tail -1)

    if [ -z "${ref_frame}" ] || [ ! -f "${rw_state}" ] || [ -z "${rw_frame}" ]; then
        echo "   FAIL: ${rom_name} rewind (missing frames or state file)"
        FAIL=$((FAIL + 1))
        SUMMARY="${SUMMARY}| ${rom_name} (rewind) | :x: FAIL | missing output | - |\n"
    elif cmp -s "${ref_frame}" "${rw_frame}"; then
        echo "   PASS: ${rom_name} rewind (resume from frame ${RW_REWIND_TO} matches)"
        PASS=$((PASS + 1))
        SUMMARY="${SUMMARY}| ${rom_name} (rewind) | :white_check_mark: PASS | rewind matches | - |\n"
    else
        echo "   FAIL: ${rom_name} rewind (resume from frame ${RW_REWIND_TO} differs)"
        cp "${ref_frame}" "${DIFF_DIR}/${rom_name}_rw_ref.png"
        cp "${rw_frame}" "${DIFF_DIR}/${rom_name}_rw_resume.png"
        FAIL=$((FAIL + 1))
        SUMMARY="${SUMMARY}| ${rom_name} (rewind) | :x: FAIL | rewind mismatch | See artifacts |\n"
    fi
done

echo ""
echo "==> Results: ${PASS} passed, ${FAIL} failed, ${NEW} new (no baseline)"

# Write summary for CI to pick up
cat > "${DIFF_DIR}/summary.md" <<EOSUMMARY
## Regression Test Results

| ROM | Status | Details | Diff |
|-----|--------|---------|------|
$(printf '%b' "${SUMMARY}")

**Platform:** $(uname -s) $(uname -m)
EOSUMMARY

if [ "${FAIL}" -gt 0 ]; then
    exit 1
fi
