#!/bin/bash
#
# Unified vector benchmark runner for LSH / Pivot / Hybrid / Annoy / Rotation-family
# - common evaluator via ceph-vector-bench
# - stage1: broad sweep with small pivot samples
# - stage2: recheck shortlisted Pivot/Hybrid configs with large pivot samples
#
# Usage:
#   ./run_vector_bench_compare.sh
#   ./run_vector_bench_compare.sh --reset-vstart
#   ./run_vector_bench_compare.sh --only-stage1
#   ./run_vector_bench_compare.sh --only-stage2
#   ./run_vector_bench_compare.sh --only-backends pivot,hybrid
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

LOG_FILE="${LOG_FILE:-benchmark_results_compare.txt}"
LOG_PATH="${SCRIPT_DIR}/${LOG_FILE}"

DATA_DIR="${DATA_DIR:-${SCRIPT_DIR}/sift1M/sift}"
BASE_FVECS="${BASE_FVECS:-${DATA_DIR}/sift_base.fvecs}"
QUERY_FVECS="${QUERY_FVECS:-${DATA_DIR}/sift_query.fvecs}"
GT_IVECS="${GT_IVECS:-${DATA_DIR}/sift_groundtruth.ivecs}"

BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build}"
BENCH_BIN="${BENCH_BIN:-${BUILD_DIR}/bin/ceph-vector-bench}"
CEPH_BIN="${CEPH_BIN:-${BUILD_DIR}/bin/ceph}"
VSTART_DEST="${VSTART_DEST:-/tmp/ceph-vstart-vector}"
export VSTART_DEST
export CEPH_CONF="${CEPH_CONF:-${VSTART_DEST}/ceph.conf}"

PG_NUM=1024
QUERY_NUM_STAGE1="${QUERY_NUM_STAGE1:-100}"
QUERY_NUM_STAGE2="${QUERY_NUM_STAGE2:-1000}"
TOPK_TARGET="${TOPK_TARGET:-100}"

RESET_VSTART=0
ONLY_STAGE1=0
ONLY_STAGE2=0
ONLY_BACKENDS_CSV="${ONLY_BACKENDS:-}"
SHOW_DF=1
VSTART_MON="${VSTART_MON:-1}"
VSTART_OSD="${VSTART_OSD:-1}"
VSTART_MGR="${VSTART_MGR:-1}"
VSTART_EXTRA_OPTS="${VSTART_EXTRA_OPTS:-}"

STAGE1_PIVOT_SAMPLES=(4096 8192 16384)
STAGE2_PIVOT_SAMPLES=(50000 65536)

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_PATH"
}

usage() {
  cat <<'EOF'
usage: run_vector_bench_compare.sh [options]

options:
  --reset-vstart            stop local cluster, wipe VSTART_DEST, then start memstore vstart with high PG limits
  --only-stage1             run stage1 only
  --only-stage2             run stage2 only
  --only-backends <csv>     comma-separated subset: lsh,pivot,hybrid,annoy,rotation_repr1,faiss_rotation
  --no-df                   skip df -h after vstart reset
  --help, -h                show help

notes:
  - pg_num is fixed to 1024
  - pivot count is implicitly fixed to pg_num (=1024)
  - pivot_build_sample is 2-stage:
      stage1 = 4096,8192,16384
      stage2 = 50000,65536
  - DATA_DIR defaults to /home/dev_path/ceph/sift1M/sift
  - VSTART_DEST defaults to /tmp/ceph-vstart-vector
EOF
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --reset-vstart)
        RESET_VSTART=1
        shift
        ;;
      --only-stage1)
        ONLY_STAGE1=1
        shift
        ;;
      --only-stage2)
        ONLY_STAGE2=1
        shift
        ;;
      --only-backends)
        ONLY_BACKENDS_CSV="${2:-}"
        shift 2
        ;;
      --no-df)
        SHOW_DF=0
        shift
        ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        echo "unknown option: $1" >&2
        usage
        exit 1
        ;;
    esac
  done
  if [[ "$ONLY_STAGE1" == "1" && "$ONLY_STAGE2" == "1" ]]; then
    echo "cannot set both --only-stage1 and --only-stage2" >&2
    exit 1
  fi
}

check_files() {
  for f in "$BASE_FVECS" "$QUERY_FVECS" "$GT_IVECS" "$BENCH_BIN" "$CEPH_BIN"; do
    if [[ ! -f "$f" ]]; then
      echo "missing file: $f" >&2
      exit 1
    fi
  done
}

backend_enabled() {
  local target="$1"
  if [[ -z "$ONLY_BACKENDS_CSV" ]]; then
    return 0
  fi
  local token
  IFS=',' read -r -a _BACKENDS <<< "$ONLY_BACKENDS_CSV"
  for token in "${_BACKENDS[@]}"; do
    if [[ "$token" == "$target" ]]; then
      return 0
    fi
  done
  return 1
}

reset_vstart_cluster() {
  log "========== vstart reset start =========="
  log "build_dir=${BUILD_DIR}, vstart_dest=${VSTART_DEST}, MON=${VSTART_MON}, OSD=${VSTART_OSD}, MGR=${VSTART_MGR}"
  mkdir -p "$BUILD_DIR"
  (
    cd "$BUILD_DIR"
    env -u CEPH_BIN -u CEPH_ADM -u INIT_CEPH "${SCRIPT_DIR}/src/stop.sh" || true
    rm -rf "$VSTART_DEST"
    env -u CEPH_BIN -u CEPH_ADM -u INIT_CEPH \
      VSTART_DEST="$VSTART_DEST" MON="$VSTART_MON" OSD="$VSTART_OSD" MGR="$VSTART_MGR" MDS=0 RGW=0 NFS=0 \
      "${SCRIPT_DIR}/src/vstart.sh" -n -l -X --memstore --without-dashboard --nolockdep \
      -o 'osd_pool_default_size=1' \
      -o 'osd_pool_default_min_size=1' \
      -o 'mon_max_pg_per_osd=10000' \
      -o 'mon_pg_warn_max_per_osd=10000' \
      -o 'osd_max_pg_per_osd_hard_ratio=100' \
      $VSTART_EXTRA_OPTS
    if [[ "$SHOW_DF" == "1" ]]; then
      df -h
    fi
  )
  log "========== vstart reset end =========="
}

allow_large_pg_and_pool_delete() {
  "$CEPH_BIN" tell mon.* injectargs '--mon-allow-pool-delete=true' >>"$LOG_PATH" 2>&1 || true
  "$CEPH_BIN" tell mon.* injectargs '--mon_max_pg_per_osd=10000 --mon_pg_warn_max_per_osd=10000' >>"$LOG_PATH" 2>&1 || true
  "$CEPH_BIN" tell osd.* injectargs '--osd_max_pg_per_osd_hard_ratio=100' >>"$LOG_PATH" 2>&1 || true
}

prepare_pool() {
  local pool="$1"
  "$CEPH_BIN" osd pool delete "$pool" "$pool" --yes-i-really-really-mean-it >>"$LOG_PATH" 2>&1 || true
  "$CEPH_BIN" osd pool create "$pool" "$PG_NUM" "$PG_NUM" >>"$LOG_PATH" 2>&1
  "$CEPH_BIN" osd pool set "$pool" pg_autoscale_mode off >>"$LOG_PATH" 2>&1
}

cleanup_pool() {
  local pool="$1"
  "$CEPH_BIN" osd pool delete "$pool" "$pool" --yes-i-really-really-mean-it >>"$LOG_PATH" 2>&1 || true
}

run_case() {
  local stage="$1"
  local backend="$2"
  local case_id="$3"
  local query_num="$4"
  shift 4
  local extra_args=("$@")

  local pool="vec_${backend}_${stage}_${case_id}"
  log "case start: stage=${stage} backend=${backend} case=${case_id} pool=${pool}"
  log "args: ${extra_args[*]}"

  prepare_pool "$pool"

  if ! "$BENCH_BIN" --file "$BASE_FVECS" --pool "$pool" --pg-num "$PG_NUM" "${extra_args[@]}" >>"$LOG_PATH" 2>&1; then
    log "load failed: stage=${stage} backend=${backend} case=${case_id}"
    cleanup_pool "$pool"
    return 1
  fi

  if ! "$BENCH_BIN" --recall \
    --file "$BASE_FVECS" \
    --pool "$pool" \
    --query "$QUERY_FVECS" \
    --gt "$GT_IVECS" \
    --pg-num "$PG_NUM" \
    --query-num "$query_num" \
    --qps-mode memory \
    --latency-report \
    "${extra_args[@]}" >>"$LOG_PATH" 2>&1; then
    log "recall failed: stage=${stage} backend=${backend} case=${case_id}"
    cleanup_pool "$pool"
    return 1
  fi

  cleanup_pool "$pool"
  log "case done: stage=${stage} backend=${backend} case=${case_id}"
}

run_stage1_lsh() {
  backend_enabled lsh || return 0
  local tables write probe case_id
  for tables in 8 16 32; do
    for write in 1 2 3 4; do
      for probe in 3 4 5 6 7 8; do
        case_id="pg${PG_NUM}_t${tables}_w${write}_p${probe}"
        run_case stage1 lsh "$case_id" "$QUERY_NUM_STAGE1" \
          --hash-backend lsh \
          --num-tables "$tables" \
          --write-top-pgs "$write" \
          --probe-mode vote \
          --probe-pgs "$probe"
      done
    done
  done
}

run_stage1_pivot() {
  backend_enabled pivot || return 0
  local sample write probe budget case_id
  for sample in "${STAGE1_PIVOT_SAMPLES[@]}"; do
    for budget in 4 6 8; do
      for write in 1 2 3 4; do
        for probe in 3 4 5 6 7 8; do
          case_id="pg${PG_NUM}_s${sample}_b${budget}_w${write}_p${probe}"
          run_case stage1 pivot "$case_id" "$QUERY_NUM_STAGE1" \
            --hash-backend pivot \
            --pivot-build-sample "$sample" \
            --pivot-probe-budget "$budget" \
            --write-top-pgs "$write" \
            --probe-mode vote \
            --probe-pgs "$probe"
        done
      done
    done
  done
}

run_stage1_hybrid() {
  backend_enabled hybrid || return 0
  local sample tables lsh_topk write probe case_id
  for sample in "${STAGE1_PIVOT_SAMPLES[@]}"; do
    for tables in 8 16 32; do
      for lsh_topk in 4 8 12; do
        for write in 2 3 4; do
          for probe in 4 5 6 7 8; do
            case_id="pg${PG_NUM}_s${sample}_t${tables}_lk${lsh_topk}_w${write}_p${probe}"
            run_case stage1 hybrid "$case_id" "$QUERY_NUM_STAGE1" \
              --hash-backend hybrid \
              --num-tables "$tables" \
              --pivot-build-sample "$sample" \
              --hybrid-lsh-vote-topk "$lsh_topk" \
              --write-top-pgs "$write" \
              --probe-mode vote \
              --probe-pgs "$probe"
          done
        done
      done
    done
  done
}

run_stage1_annoy() {
  backend_enabled annoy || return 0
  local trees search_k write probe sample case_id
  for trees in 8 16 32; do
    for search_k in 128 256 512; do
      for sample in 100000 200000; do
        for write in 1 2 3; do
          for probe in 6 7 8 9 10; do
            case_id="pg${PG_NUM}_tr${trees}_sk${search_k}_bs${sample}_w${write}_p${probe}"
            run_case stage1 annoy "$case_id" "$QUERY_NUM_STAGE1" \
              --hash-backend annoy \
              --annoy-n-trees "$trees" \
              --annoy-search-k "$search_k" \
              --annoy-build-sample "$sample" \
              --write-top-pgs "$write" \
              --probe-mode vote \
              --probe-pgs "$probe"
          done
        done
      done
    done
  done
}

run_stage1_rotation_repr1() {
  backend_enabled rotation_repr1 || return 0
  local write probe seed case_id
  for seed in 11 22 33; do
    for write in 1 2 3; do
      for probe in 4 6 8; do
        case_id="pg${PG_NUM}_seed${seed}_w${write}_p${probe}"
        run_case stage1 rotation_repr1 "$case_id" "$QUERY_NUM_STAGE1" \
          --hash-backend rotation_repr1 \
          --rot-seed "$seed" \
          --write-top-pgs "$write" \
          --probe-mode vote \
          --probe-pgs "$probe"
      done
    done
  done
}

run_stage1_faiss_rotation() {
  backend_enabled faiss_rotation || return 0
  local use_dims bins step write probe seed case_id
  for seed in 11 22 33; do
    for use_dims in 2 3 4; do
      for bins in 8 16; do
        for step in 0 1; do
          for write in 1 2 3; do
            for probe in 4 6 8; do
              case_id="pg${PG_NUM}_seed${seed}_ud${use_dims}_b${bins}_ns${step}_w${write}_p${probe}"
              run_case stage1 faiss_rotation "$case_id" "$QUERY_NUM_STAGE1" \
                --hash-backend faiss_rotation \
                --rot-seed "$seed" \
                --rotation-use-dims "$use_dims" \
                --rotation-bins-per-dim "$bins" \
                --rotation-neighbor-step "$step" \
                --write-top-pgs "$write" \
                --probe-mode vote \
                --probe-pgs "$probe"
            done
          done
        done
      done
    done
  done
}

run_stage2_pivot() {
  backend_enabled pivot || return 0
  local sample budget write probe case_id
  for sample in "${STAGE2_PIVOT_SAMPLES[@]}"; do
    for budget in 6 8; do
      for write in 2 3 4; do
        for probe in 4 5 6 7; do
          case_id="pg${PG_NUM}_s${sample}_b${budget}_w${write}_p${probe}"
          run_case stage2 pivot "$case_id" "$QUERY_NUM_STAGE2" \
            --hash-backend pivot \
            --pivot-build-sample "$sample" \
            --pivot-probe-budget "$budget" \
            --write-top-pgs "$write" \
            --probe-mode vote \
            --probe-pgs "$probe"
        done
      done
    done
  done
}

run_stage2_hybrid() {
  backend_enabled hybrid || return 0
  local sample tables lsh_topk write probe case_id
  for sample in "${STAGE2_PIVOT_SAMPLES[@]}"; do
    for tables in 8 16; do
      for lsh_topk in 4 8; do
        for write in 2 3 4; do
          for probe in 4 5 6 7; do
            case_id="pg${PG_NUM}_s${sample}_t${tables}_lk${lsh_topk}_w${write}_p${probe}"
            run_case stage2 hybrid "$case_id" "$QUERY_NUM_STAGE2" \
              --hash-backend hybrid \
              --num-tables "$tables" \
              --pivot-build-sample "$sample" \
              --hybrid-lsh-vote-topk "$lsh_topk" \
              --write-top-pgs "$write" \
              --probe-mode vote \
              --probe-pgs "$probe"
          done
        done
      done
    done
  done
}

run_stage1() {
  run_stage1_lsh
  run_stage1_pivot
  run_stage1_hybrid
  run_stage1_annoy
  run_stage1_rotation_repr1
  run_stage1_faiss_rotation
}

run_stage2() {
  run_stage2_pivot
  run_stage2_hybrid
}

main() {
  parse_args "$@"
  check_files
  : > "$LOG_PATH"
  log "========== compare benchmark start =========="
  log "pg_num=${PG_NUM}, topk_target=${TOPK_TARGET}, stage1_query_num=${QUERY_NUM_STAGE1}, stage2_query_num=${QUERY_NUM_STAGE2}"

  if [[ "$RESET_VSTART" == "1" ]]; then
    reset_vstart_cluster
  fi
  allow_large_pg_and_pool_delete

  if [[ "$ONLY_STAGE2" != "1" ]]; then
    run_stage1
  fi
  if [[ "$ONLY_STAGE1" != "1" ]]; then
    run_stage2
  fi

  log "========== compare benchmark end =========="
}

main "$@"
