#!/bin/bash
#
# LSH vs Orthogonal Rotation sweep runner
# - 기존 run_vector_bench_set_paired.sh 를 래핑하여
#   hash backend / seed 반복 실행을 자동화한다.
#
# 사용 예시:
#   ./run_vector_bench_orth_rot_sweep.sh
#   ./run_vector_bench_orth_rot_sweep.sh --orth-only
#   ORTH_ROT_SEEDS="11,22,33,44,55" ./run_vector_bench_orth_rot_sweep.sh
#   HASH_BITS=10 ./run_vector_bench_orth_rot_sweep.sh --background
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_SCRIPT="${SCRIPT_DIR}/run_vector_bench_set_paired.sh"
LOG_FILE="${SCRIPT_DIR}/benchmark_results_orth_rot_sweep.txt"

# defaults (env override 가능)
RUN_LSH=1
RUN_ORTH=1
ORTH_ROT_SEEDS_CSV="${ORTH_ROT_SEEDS:-11,22,33,44,55}"
HASH_BITS="${HASH_BITS:-0}"      # 0 => pg_num 기반 자동
LSH_ROT_SEED="${LSH_ROT_SEED:-1315423911}"
HASH_REPEAT_ROUNDS="${HASH_REPEAT_ROUNDS:-1}"
REPEAT_SEED_STRIDE="${REPEAT_SEED_STRIDE:-2654435761}"
REPEAT_SELECT_SINGLE_PG="${REPEAT_SELECT_SINGLE_PG:-0}"

BACKGROUND_MODE=0
PASS_THROUGH_ARGS=()

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

usage() {
  cat <<'EOF'
usage: run_vector_bench_orth_rot_sweep.sh [options]

options:
  --lsh-only          LSH만 실행
  --orth-only         orth-rot만 실행
  --background, -b    내부 벤치 스크립트를 백그라운드 모드로 실행
  -h, --help          도움말

environment variables:
  ORTH_ROT_SEEDS   orth seed 목록 (comma-separated), default: 11,22,33,44,55
  HASH_BITS        hash bit-width (0=auto), default: 0
  HASH_REPEAT_ROUNDS   repeated rounds for orth-rot, default: 1
  REPEAT_SEED_STRIDE   seed stride per round, default: 2654435761
  REPEAT_SELECT_SINGLE_PG 1이면 top-1 PG 강제 선택, default: 0
  LSH_ROT_SEED     lsh run에서도 전달할 seed 값(비교용 로그 통일), default: 1315423911

note:
  상세 스윕 파라미터(PG_NUM_ARRAY, TABLE_SET_SIZE_ARRAY 등)는
  run_vector_bench_set_paired.sh 내부 설정을 그대로 따른다.
EOF
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --lsh-only)
        RUN_LSH=1
        RUN_ORTH=0
        shift
        ;;
      --orth-only)
        RUN_LSH=0
        RUN_ORTH=1
        shift
        ;;
      --background|-b)
        BACKGROUND_MODE=1
        PASS_THROUGH_ARGS+=("--background")
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        PASS_THROUGH_ARGS+=("$1")
        shift
        ;;
    esac
  done
}

split_csv_to_array() {
  local csv="$1"
  local -n out_ref=$2
  out_ref=()
  IFS=',' read -r -a out_ref <<< "$csv"
}

run_case() {
  local backend="$1"
  local seed="$2"
  local extra_args="--hash-repeat-rounds ${HASH_REPEAT_ROUNDS} --repeat-seed-stride ${REPEAT_SEED_STRIDE}"
  if [[ "$REPEAT_SELECT_SINGLE_PG" == "1" ]]; then
    extra_args="${extra_args} --repeat-select-single-pg"
  fi
  local mode_note=""
  if [[ "$BACKGROUND_MODE" -eq 1 ]]; then
    mode_note="(background)"
  else
    mode_note="(foreground)"
  fi

  log "------------------------------------------------------------"
  log "start case: backend=${backend}, seed=${seed}, hash_bits=${HASH_BITS} ${mode_note}"
  log "repeat_rounds=${HASH_REPEAT_ROUNDS}, repeat_seed_stride=${REPEAT_SEED_STRIDE}, single_pg=${REPEAT_SELECT_SINGLE_PG}"
  log "pass-through args: ${PASS_THROUGH_ARGS[*]:-(none)}"

  HASH_BACKEND="$backend" \
  ROT_SEED="$seed" \
  HASH_BITS="$HASH_BITS" \
  EXTRA_BENCH_ARGS="$extra_args" \
  "$BASE_SCRIPT" "${PASS_THROUGH_ARGS[@]}"

  log "done case : backend=${backend}, seed=${seed}"
}

main() {
  parse_args "$@"

  if [[ ! -x "$BASE_SCRIPT" ]]; then
    log "error: base script not executable: $BASE_SCRIPT"
    log "hint : chmod +x \"$BASE_SCRIPT\""
    exit 1
  fi

  local orth_seeds=()
  split_csv_to_array "$ORTH_ROT_SEEDS_CSV" orth_seeds
  if [[ "${#orth_seeds[@]}" -eq 0 ]]; then
    log "error: ORTH_ROT_SEEDS is empty"
    exit 1
  fi

  log "========== orth-rot sweep start =========="
  log "base script: $BASE_SCRIPT"
  log "run_lsh=${RUN_LSH}, run_orth=${RUN_ORTH}"
  log "orth seeds: ${orth_seeds[*]}"
  log "hash_bits: ${HASH_BITS}"

  if [[ "$RUN_LSH" -eq 1 ]]; then
    run_case "lsh" "$LSH_ROT_SEED"
  fi

  if [[ "$RUN_ORTH" -eq 1 ]]; then
    local s
    for s in "${orth_seeds[@]}"; do
      run_case "orth-rot" "$s"
    done
  fi

  log "========== orth-rot sweep end =========="
}

main "$@"

