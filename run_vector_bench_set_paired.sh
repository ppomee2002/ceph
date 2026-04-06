#!/bin/bash
#
# Ceph 벡터 LSH 벤치마크 (Set-based + paired replication)
#
# 사용법:
#   (포그라운드) ./run_vector_bench_set_paired.sh
#   (재개+백그라운드) START_AT_CASE="1024,8,or,8,2" ./run_vector_bench_set_paired.sh --background
#   로그: 스크립트와 같은 디렉터리의 benchmark_results_set_paired.txt (append)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="${BASH_SOURCE[0]}"
# 항상 스크립트 디렉터리에서 동작 (로그·ceph 경로 일치)
cd "$SCRIPT_DIR"

LOG_FILE="benchmark_results_set_paired.txt"
LOG_PATH="${SCRIPT_DIR}/${LOG_FILE}"
PG_NUM_ARRAY=(1024)
NUM_TABLES=32
TABLE_SET_SIZE_ARRAY=(8 16)
SET_COMBINE_ARRAY=("or")
SET_REPLICA_MODE="paired"
WRITE_TOP_PGS_PER_SET_ARRAY=(1 2 4 8)
PROBE_PGS_PER_SET_ARRAY=(1 2 4 8)
QUERY_NUM=100
PG_MAP_MODE="stable"
TABLE_COMBINE="or"
HASH_BACKEND="${HASH_BACKEND:-lsh}"   # lsh | orth-rot
ROT_SEED="${ROT_SEED:-1315423911}"
HASH_BITS="${HASH_BITS:-0}"           # 0 => auto from pg_num
EXTRA_BENCH_ARGS="${EXTRA_BENCH_ARGS:-}"
EXTRA_BENCH_ARGS_ARR=()
if [[ -n "$EXTRA_BENCH_ARGS" ]]; then
  # shellcheck disable=SC2206
  EXTRA_BENCH_ARGS_ARR=($EXTRA_BENCH_ARGS)
fi

# Resume controls (optional)
# - START_AT_CASE: start from this exact case (inclusive)
# - START_AFTER_CASE: skip up to and including this case, then start from the next one
# Case format: "PG_NUM,TABLE_SET_SIZE,SET_COMBINE,WRITE_TOP_PGS_PER_SET,PROBE_PGS_PER_SET"
# Example:
#   START_AFTER_CASE="1024,8,or,4,4" ./run_vector_bench_set_paired.sh
#
# Background:
#   START_AT_CASE="1024,8,or,8,2" ./run_vector_bench_set_paired.sh --background
#   (로그는 LOG_FILE 에 append; 터미널은 PID만 출력)
START_AT_CASE="${START_AT_CASE:-}"
START_AFTER_CASE="${START_AFTER_CASE:-}"

# Set by parent when re-invoking after --background (avoid nested detach)
_BENCH_SET_PAIRED_INNER="${_BENCH_SET_PAIRED_INNER:-}"

DATA_DIR="${SCRIPT_DIR}/siftsmall"
BASE_FVECS="${DATA_DIR}/siftsmall_base.fvecs"
QUERY_FVECS="${DATA_DIR}/siftsmall_query.fvecs"
GT_IVECS="${DATA_DIR}/siftsmall_groundtruth.ivecs"

BUILD_DIR="${SCRIPT_DIR}/build"
BENCH_BIN="${BUILD_DIR}/bin/ceph-vector-bench"
CEPH_BIN="${BUILD_DIR}/bin/ceph"

export CEPH_CONF="${BUILD_DIR}/ceph.conf"

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

case_id() {
  local pg="$1" tss="$2" sc="$3" w="$4" p="$5"
  echo "${pg},${tss},${sc},${w},${p}"
}

check_data_files() {
  for f in "$BASE_FVECS" "$QUERY_FVECS" "$GT_IVECS"; do
    if [[ ! -f "$f" ]]; then
      log "오류: 데이터 파일 없음: $f (실행 디렉터리: $(pwd))"
      exit 1
    fi
  done
}

main() {
  log "========== set-based(paired) 벤치마크 시작 =========="
  check_data_files

  $CEPH_BIN tell mon.* injectargs '--mon-allow-pool-delete=true' 2>>"$LOG_PATH" || true

  log "----------------------------------------"
  log ">>> table-combine=${TABLE_COMBINE}, pg-map-mode=${PG_MAP_MODE}"
  log ">>> hash-backend=${HASH_BACKEND}, rot-seed=${ROT_SEED}, hash-bits=${HASH_BITS}"
  if [[ -n "$EXTRA_BENCH_ARGS" ]]; then
    log ">>> extra-bench-args=${EXTRA_BENCH_ARGS}"
  fi
  log ">>> pg-num 스윕: ${PG_NUM_ARRAY[*]}"
  log ">>> table-set-size 스윕: ${TABLE_SET_SIZE_ARRAY[*]}"
  log ">>> set-combine 스윕: ${SET_COMBINE_ARRAY[*]}"
  log ">>> write-top-pgs-per-set 스윕: ${WRITE_TOP_PGS_PER_SET_ARRAY[*]}"
  log ">>> probe-pgs-per-set 스윕: ${PROBE_PGS_PER_SET_ARRAY[*]}"
  if [[ -n "$START_AT_CASE" ]]; then
    log ">>> RESUME: START_AT_CASE=${START_AT_CASE}"
  fi
  if [[ -n "$START_AFTER_CASE" ]]; then
    log ">>> RESUME: START_AFTER_CASE=${START_AFTER_CASE}"
  fi
  log "----------------------------------------"

  local started=0
  for PG_NUM in "${PG_NUM_ARRAY[@]}"; do
    for TABLE_SET_SIZE in "${TABLE_SET_SIZE_ARRAY[@]}"; do
      for SET_COMBINE in "${SET_COMBINE_ARRAY[@]}"; do
        for WRITE_TOP_PGS_PER_SET in "${WRITE_TOP_PGS_PER_SET_ARRAY[@]}"; do
          for PROBE_PGS_PER_SET in "${PROBE_PGS_PER_SET_ARRAY[@]}"; do
            local this_case
            this_case="$(case_id "$PG_NUM" "$TABLE_SET_SIZE" "$SET_COMBINE" "$WRITE_TOP_PGS_PER_SET" "$PROBE_PGS_PER_SET")"

            if [[ -n "$START_AT_CASE" && "$started" -eq 0 ]]; then
              if [[ "$this_case" == "$START_AT_CASE" ]]; then
                started=1
              else
                continue
              fi
            fi
            if [[ -n "$START_AFTER_CASE" && "$started" -eq 0 ]]; then
              if [[ "$this_case" == "$START_AFTER_CASE" ]]; then
                started=1
                continue
              else
                continue
              fi
            fi

            POOL_NAME="vector_pool_set_t${NUM_TABLES}_s${TABLE_SET_SIZE}_sc${SET_COMBINE}_w${WRITE_TOP_PGS_PER_SET}_p${PROBE_PGS_PER_SET}_pg${PG_NUM}"

            log "========================================"
            log ">>> case=${this_case}"
            log ">>> pg-num=${PG_NUM}, pool=${POOL_NAME}"
            log ">>> num-tables=${NUM_TABLES}, table-set-size=${TABLE_SET_SIZE}"
            log ">>> set-combine=${SET_COMBINE}, set-replica-mode=${SET_REPLICA_MODE}"
            log ">>> write-top-pgs-per-set=${WRITE_TOP_PGS_PER_SET}, probe-pgs-per-set=${PROBE_PGS_PER_SET}"
            log "========================================"

            log "[0단계] 기존 풀 정리 시도: ${POOL_NAME}"
            $CEPH_BIN osd pool delete "$POOL_NAME" "$POOL_NAME" --yes-i-really-really-mean-it 2>>"$LOG_PATH" || true

            log "[1단계-준비] 풀 생성: ${POOL_NAME} (pg_num=${PG_NUM})"
            if ! $CEPH_BIN osd pool create "$POOL_NAME" "$PG_NUM" "$PG_NUM" 2>>"$LOG_PATH"; then
              log "경고: 풀 생성 실패, 다음 케이스로 진행"
              continue
            fi

            log "[1단계-준비] pg_autoscale_mode off"
            if ! $CEPH_BIN osd pool set "$POOL_NAME" pg_autoscale_mode off 2>>"$LOG_PATH"; then
              log "경고: pg_autoscale_mode 설정 실패 -> 해당 케이스 중단"
              $CEPH_BIN osd pool delete "$POOL_NAME" "$POOL_NAME" --yes-i-really-really-mean-it 2>>"$LOG_PATH" || true
              continue
            fi

            log "[2단계] Load 실행 (set-based)"
            if ! $BENCH_BIN --file "$BASE_FVECS" --pool "$POOL_NAME" \
              --num-tables "$NUM_TABLES" --pg-num "$PG_NUM" \
              --table-combine "$TABLE_COMBINE" --pg-map-mode "$PG_MAP_MODE" \
              --hash-backend "$HASH_BACKEND" --rot-seed "$ROT_SEED" --hash-bits "$HASH_BITS" \
              --table-set-size "$TABLE_SET_SIZE" --set-combine "$SET_COMBINE" \
              --set-replica-mode "$SET_REPLICA_MODE" \
              --write-top-pgs-per-set "$WRITE_TOP_PGS_PER_SET" \
              "${EXTRA_BENCH_ARGS_ARR[@]}" \
              2>>"$LOG_PATH" >>"$LOG_PATH"; then
              log "경고: Load 실패, 다음 케이스로 진행"
              $CEPH_BIN osd pool delete "$POOL_NAME" "$POOL_NAME" --yes-i-really-really-mean-it 2>>"$LOG_PATH" || true
              continue
            fi

            log "[3단계] Recall 실행 (set-based)"
            if ! $BENCH_BIN --recall --file "$BASE_FVECS" --pool "$POOL_NAME" \
              --query "$QUERY_FVECS" --gt "$GT_IVECS" --query-num "$QUERY_NUM" \
              --num-tables "$NUM_TABLES" --pg-num "$PG_NUM" \
              --table-combine "$TABLE_COMBINE" --pg-map-mode "$PG_MAP_MODE" \
              --hash-backend "$HASH_BACKEND" --rot-seed "$ROT_SEED" --hash-bits "$HASH_BITS" \
              --table-set-size "$TABLE_SET_SIZE" --set-combine "$SET_COMBINE" \
              --set-replica-mode "$SET_REPLICA_MODE" \
              --probe-pgs-per-set "$PROBE_PGS_PER_SET" \
              "${EXTRA_BENCH_ARGS_ARR[@]}" \
              2>>"$LOG_PATH" >>"$LOG_PATH"; then
              log "경고: Recall 실패 (pg-num=${PG_NUM})"
            fi

            log "[4단계] 풀 삭제: ${POOL_NAME}"
            $CEPH_BIN osd pool delete "$POOL_NAME" "$POOL_NAME" --yes-i-really-really-mean-it 2>>"$LOG_PATH" || true
          done
        done
      done
    done
  done

  log "========== set-based(paired) 벤치마크 종료 =========="
}

if [[ "${1:-}" == "--background" || "${1:-}" == "-b" ]]; then
  shift
  # stdin / SIGHUP 에 덜 묶이도록 분리
  nohup env \
    START_AT_CASE="${START_AT_CASE}" \
    START_AFTER_CASE="${START_AFTER_CASE}" \
    _BENCH_SET_PAIRED_INNER=1 \
    bash "$SELF" "$@" >>"$LOG_PATH" 2>&1 </dev/null &
  echo "run_vector_bench_set_paired: background PID $!  log=$LOG_PATH"
  exit 0
fi

if [[ "${_BENCH_SET_PAIRED_INNER:-}" == 1 ]]; then
  main
else
  main 2>&1 | tee -a "$LOG_PATH"
fi
exit 0
