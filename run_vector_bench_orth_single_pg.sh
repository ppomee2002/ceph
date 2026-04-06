#!/bin/bash
#
# Orthogonal-rotation only benchmark (single PG selection mode)
# - 의도: "적용(apply) 단계에서 repeat_rounds 만큼 회전-해시 후 투표"
#
# 기본 동작:
#   - hash-backend: orth-rot
#   - num-tables: 1
#   - hash-repeat-rounds: 8 (env override 가능)
#   - repeat-select-single-pg: enabled
#   - table-set-size: 0 (legacy all-table path)
#
# 사용 예시:
#   ./run_vector_bench_orth_single_pg.sh
#   ORTH_ROT_SEEDS="11,22,33,44,55" HASH_REPEAT_ROUNDS=16 ./run_vector_bench_orth_single_pg.sh
#   QUERY_NUM=200 PG_NUM=1024 ./run_vector_bench_orth_single_pg.sh --background
#   SWEEP_MODE=1 PG_NUM=1024 QUERY_NUM=100 ./run_vector_bench_orth_single_pg.sh --background
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="${BASH_SOURCE[0]}"
cd "$SCRIPT_DIR"

LOG_FILE="benchmark_results_orth_single_pg.txt"
LOG_PATH="${SCRIPT_DIR}/${LOG_FILE}"

DATA_DIR="${SCRIPT_DIR}/siftsmall"
BASE_FVECS="${DATA_DIR}/siftsmall_base.fvecs"
QUERY_FVECS="${DATA_DIR}/siftsmall_query.fvecs"
GT_IVECS="${DATA_DIR}/siftsmall_groundtruth.ivecs"

BUILD_DIR="${SCRIPT_DIR}/build"
BENCH_BIN="${BUILD_DIR}/bin/ceph-vector-bench"
CEPH_BIN="${BUILD_DIR}/bin/ceph"
export CEPH_CONF="${BUILD_DIR}/ceph.conf"

# defaults (env override 가능)
PG_NUM="${PG_NUM:-128}"
NUM_TABLES="${NUM_TABLES:-1}"
QUERY_NUM="${QUERY_NUM:-100}"
PG_MAP_MODE="${PG_MAP_MODE:-stable}"
TABLE_COMBINE="${TABLE_COMBINE:-or}"
PROBE_MODE="${PROBE_MODE:-vote}"
PROBE_PGS="${PROBE_PGS:-1}"
WRITE_TOP_PGS="${WRITE_TOP_PGS:-3}"
HASH_BITS="${HASH_BITS:-0}"
HASH_REPEAT_ROUNDS="${HASH_REPEAT_ROUNDS:-8}"
REPEAT_SEED_STRIDE="${REPEAT_SEED_STRIDE:-2654435761}"
ORTH_ROT_SEEDS="${ORTH_ROT_SEEDS:-11,22,33,44,55}"
REPEAT_SELECT_SINGLE_PG="${REPEAT_SELECT_SINGLE_PG:-1}"
SWEEP_MODE="${SWEEP_MODE:-0}"
SWEEP_NUM_TABLES="${SWEEP_NUM_TABLES:-8,16}"
SWEEP_HASH_REPEAT_ROUNDS="${SWEEP_HASH_REPEAT_ROUNDS:-1,2}"
SWEEP_WRITE_TOP_PGS="${SWEEP_WRITE_TOP_PGS:-3,6}"
SWEEP_PROBE_PGS="${SWEEP_PROBE_PGS:-2,4,8}"
SWEEP_REPEAT_SELECT_SINGLE_PG="${SWEEP_REPEAT_SELECT_SINGLE_PG:-0}"
SWEEP_ORTH_ROT_SEEDS="${SWEEP_ORTH_ROT_SEEDS:-33,66}"
SWEEP_MAX_CASES="${SWEEP_MAX_CASES:-0}"

_INNER="${_ORTH_SINGLE_PG_INNER:-0}"

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

check_files() {
  for f in "$BASE_FVECS" "$QUERY_FVECS" "$GT_IVECS" "$BENCH_BIN" "$CEPH_BIN"; do
    if [[ ! -f "$f" ]]; then
      log "오류: 파일 없음: $f"
      exit 1
    fi
  done
}

split_csv() {
  local csv="$1"
  local -n out="$2"
  out=()
  IFS=',' read -r -a out <<< "$csv"
}

is_truthy() {
  local v="${1,,}"
  [[ "$v" == "1" || "$v" == "true" || "$v" == "yes" || "$v" == "on" ]]
}

bool_to_label() {
  if is_truthy "$1"; then
    echo "1"
  else
    echo "0"
  fi
}

run_one_seed() {
  local seed="$1"
  local single_pg_enabled="false"
  local repeat_single_pg_flag=()
  if is_truthy "$REPEAT_SELECT_SINGLE_PG"; then
    single_pg_enabled="true"
    repeat_single_pg_flag+=(--repeat-select-single-pg)
  fi
  local single_pg_label
  single_pg_label="$(bool_to_label "$REPEAT_SELECT_SINGLE_PG")"
  local pool="vector_pool_orth_seed${seed}_pg${PG_NUM}_nt${NUM_TABLES}_r${HASH_REPEAT_ROUNDS}_w${WRITE_TOP_PGS}_p${PROBE_PGS}_sg${single_pg_label}"

  log "===================================================="
  log "seed=${seed}, pool=${pool}"
  log "num_tables=${NUM_TABLES}, repeat_rounds=${HASH_REPEAT_ROUNDS}, single_pg=${single_pg_enabled}, probe_mode=${PROBE_MODE}, probe_pgs=${PROBE_PGS}"
  log "===================================================="

  $CEPH_BIN osd pool delete "$pool" "$pool" --yes-i-really-really-mean-it 2>>"$LOG_PATH" || true
  if ! $CEPH_BIN osd pool create "$pool" "$PG_NUM" "$PG_NUM" 2>>"$LOG_PATH"; then
    log "경고: 풀 생성 실패(seed=${seed}), skip"
    return
  fi
  $CEPH_BIN osd pool set "$pool" pg_autoscale_mode off 2>>"$LOG_PATH" || true

  if ! $BENCH_BIN --file "$BASE_FVECS" --pool "$pool" \
    --num-tables "$NUM_TABLES" --pg-num "$PG_NUM" \
    --table-combine "$TABLE_COMBINE" --pg-map-mode "$PG_MAP_MODE" \
    --write-top-pgs "$WRITE_TOP_PGS" \
    --probe-mode "$PROBE_MODE" --probe-pgs "$PROBE_PGS" \
    --hash-backend orth-rot --rot-seed "$seed" --hash-bits "$HASH_BITS" \
    --hash-repeat-rounds "$HASH_REPEAT_ROUNDS" \
    --repeat-seed-stride "$REPEAT_SEED_STRIDE" \
    "${repeat_single_pg_flag[@]}" \
    2>>"$LOG_PATH" >>"$LOG_PATH"; then
    log "경고: Load 실패(seed=${seed})"
    $CEPH_BIN osd pool delete "$pool" "$pool" --yes-i-really-really-mean-it 2>>"$LOG_PATH" || true
    return
  fi

  if ! $BENCH_BIN --recall --file "$BASE_FVECS" --pool "$pool" \
    --query "$QUERY_FVECS" --gt "$GT_IVECS" --query-num "$QUERY_NUM" \
    --num-tables "$NUM_TABLES" --pg-num "$PG_NUM" \
    --table-combine "$TABLE_COMBINE" --pg-map-mode "$PG_MAP_MODE" \
    --probe-mode "$PROBE_MODE" --probe-pgs "$PROBE_PGS" \
    --hash-backend orth-rot --rot-seed "$seed" --hash-bits "$HASH_BITS" \
    --hash-repeat-rounds "$HASH_REPEAT_ROUNDS" \
    --repeat-seed-stride "$REPEAT_SEED_STRIDE" \
    "${repeat_single_pg_flag[@]}" \
    2>>"$LOG_PATH" >>"$LOG_PATH"; then
    log "경고: Recall 실패(seed=${seed})"
  fi

  $CEPH_BIN osd pool delete "$pool" "$pool" --yes-i-really-really-mean-it 2>>"$LOG_PATH" || true
}

run_single_mode() {
  check_files
  local seeds=()
  split_csv "$ORTH_ROT_SEEDS" seeds

  log "========== orth single-pg benchmark start =========="
  log "PG_NUM=${PG_NUM}, QUERY_NUM=${QUERY_NUM}, NUM_TABLES=${NUM_TABLES}"
  log "PROBE_MODE=${PROBE_MODE}, PROBE_PGS=${PROBE_PGS}, REPEAT_SELECT_SINGLE_PG=${REPEAT_SELECT_SINGLE_PG}"
  log "HASH_REPEAT_ROUNDS=${HASH_REPEAT_ROUNDS}, REPEAT_SEED_STRIDE=${REPEAT_SEED_STRIDE}"
  log "ORTH_ROT_SEEDS=${ORTH_ROT_SEEDS}"

  for s in "${seeds[@]}"; do
    run_one_seed "$s"
  done
}

run_sweep_mode() {
  check_files
  local nts=()
  local rounds=()
  local writes=()
  local probes=()
  local singles=()
  local seeds=()
  split_csv "$SWEEP_NUM_TABLES" nts
  split_csv "$SWEEP_HASH_REPEAT_ROUNDS" rounds
  split_csv "$SWEEP_WRITE_TOP_PGS" writes
  split_csv "$SWEEP_PROBE_PGS" probes
  split_csv "$SWEEP_REPEAT_SELECT_SINGLE_PG" singles
  split_csv "$SWEEP_ORTH_ROT_SEEDS" seeds

  log "========== orth sweep benchmark start =========="
  log "PG_NUM=${PG_NUM}, QUERY_NUM=${QUERY_NUM}, SWEEP_MAX_CASES=${SWEEP_MAX_CASES}"
  log "SWEEP_NUM_TABLES=${SWEEP_NUM_TABLES}"
  log "SWEEP_HASH_REPEAT_ROUNDS=${SWEEP_HASH_REPEAT_ROUNDS}"
  log "SWEEP_WRITE_TOP_PGS=${SWEEP_WRITE_TOP_PGS}"
  log "SWEEP_PROBE_PGS=${SWEEP_PROBE_PGS}"
  log "SWEEP_REPEAT_SELECT_SINGLE_PG=${SWEEP_REPEAT_SELECT_SINGLE_PG}"
  log "SWEEP_ORTH_ROT_SEEDS=${SWEEP_ORTH_ROT_SEEDS}"

  local case_idx=0
  for nt in "${nts[@]}"; do
    for rr in "${rounds[@]}"; do
      for wt in "${writes[@]}"; do
        for pp in "${probes[@]}"; do
          for sg in "${singles[@]}"; do
            if [[ "$SWEEP_MAX_CASES" -gt 0 && "$case_idx" -ge "$SWEEP_MAX_CASES" ]]; then
              log "SWEEP_MAX_CASES(${SWEEP_MAX_CASES}) reached, stop"
              log "========== orth sweep benchmark end =========="
              return
            fi

            case_idx=$((case_idx + 1))
            NUM_TABLES="$nt"
            HASH_REPEAT_ROUNDS="$rr"
            WRITE_TOP_PGS="$wt"
            PROBE_PGS="$pp"
            REPEAT_SELECT_SINGLE_PG="$sg"
            ORTH_ROT_SEEDS="$(IFS=,; echo "${seeds[*]}")"

            log "---------------- sweep case ${case_idx} ----------------"
            log "case params: num_tables=${NUM_TABLES}, repeat_rounds=${HASH_REPEAT_ROUNDS}, write_top_pgs=${WRITE_TOP_PGS}, probe_pgs=${PROBE_PGS}, single_pg=${REPEAT_SELECT_SINGLE_PG}"
            for s in "${seeds[@]}"; do
              run_one_seed "$s"
            done
          done
        done
      done
    done
  done

  log "========== orth sweep benchmark end =========="
}

main() {
  if is_truthy "$SWEEP_MODE"; then
    run_sweep_mode
  else
    run_single_mode
    log "========== orth single-pg benchmark end =========="
  fi
}

if [[ "${1:-}" == "--background" || "${1:-}" == "-b" ]]; then
  shift
  nohup env _ORTH_SINGLE_PG_INNER=1 bash "$SELF" "$@" >>"$LOG_PATH" 2>&1 </dev/null &
  echo "run_vector_bench_orth_single_pg: background PID $! log=$LOG_PATH"
  exit 0
fi

if [[ "$_INNER" == "1" ]]; then
  main
else
  main 2>&1 | tee -a "$LOG_PATH"
fi

