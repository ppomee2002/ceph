#!/bin/bash
#
# Ceph 벡터 LSH 벤치마크 (Fan-out) 자동화 스크립트
# - 기존 run_vector_bench.sh는 건드리지 않고,
# - probe-pgs를 2 -> 3 -> 4로만 테스트하는 버전
#

LOG_FILE="benchmark_results_probe_2_3_4.log"
PG_NUM=4096
NUM_TABLES=32
QUERY_NUM=100
PROBE_MODE="vote"
PROBE_PGS_ARRAY=(1 2 4 8 16 32 64 128)
WRITE_TOP_PGS_ARRAY=(1 2 4 8 16 32)

DATA_DIR="/home/dev_path/ceph/siftsmall"
BASE_FVECS="${DATA_DIR}/siftsmall_base.fvecs"
QUERY_FVECS="${DATA_DIR}/siftsmall_query.fvecs"
GT_IVECS="${DATA_DIR}/siftsmall_groundtruth.ivecs"

SCRIPT_DIR="/home/dev_path/ceph"
BUILD_DIR="${SCRIPT_DIR}/build"
BENCH_BIN="${BUILD_DIR}/bin/ceph-vector-bench"
CEPH_BIN="${BUILD_DIR}/bin/ceph"

export CEPH_CONF="${BUILD_DIR}/ceph.conf"

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
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
  log "========== 벤치마크 시작 =========="
  check_data_files

  $CEPH_BIN tell mon.* injectargs '--mon-allow-pool-delete=true' 2>>"$LOG_FILE" || true

  log "----------------------------------------"
  log ">>> num-tables=${NUM_TABLES} 고정"
  log ">>> write-top-pgs 스윕: ${WRITE_TOP_PGS_ARRAY[*]}"
  log ">>> recall 설정: probe-mode=${PROBE_MODE}, probe-pgs=${PROBE_PGS_ARRAY[*]}, query-num=${QUERY_NUM}"
  log "----------------------------------------"

  for W in "${WRITE_TOP_PGS_ARRAY[@]}"; do
    POOL_NAME="vector_pool_t${NUM_TABLES}_w${W}"
    log "========================================"
    log ">>> write-top-pgs=${W}, pool=${POOL_NAME}"
    log "========================================"

    log "[0단계] 기존 풀 정리 시도: ${POOL_NAME}"
    $CEPH_BIN osd pool delete "$POOL_NAME" "$POOL_NAME" --yes-i-really-really-mean-it 2>>"$LOG_FILE" || true

    log "[1단계-준비] 풀 생성: ${POOL_NAME} (pg_num=${PG_NUM})"
    if ! $CEPH_BIN osd pool create "$POOL_NAME" "$PG_NUM" "$PG_NUM" 2>>"$LOG_FILE"; then
      log "경고: 풀 생성 실패, 다음 케이스로 진행"
      continue
    fi

    log "[1단계-준비] pg_autoscale_mode off (필수)"
    if ! $CEPH_BIN osd pool set "$POOL_NAME" pg_autoscale_mode off 2>>"$LOG_FILE"; then
      log "경고: pg_autoscale_mode 설정 실패 -> 해당 케이스 중단"
      $CEPH_BIN osd pool delete "$POOL_NAME" "$POOL_NAME" --yes-i-really-really-mean-it 2>>"$LOG_FILE" || true
      continue
    fi
    autoscale_mode="$($CEPH_BIN osd pool get "$POOL_NAME" pg_autoscale_mode 2>>"$LOG_FILE" || true)"
    if [[ "$autoscale_mode" != *"off"* ]]; then
      log "경고: pg_autoscale_mode 검증 실패(off 아님) -> 해당 케이스 중단"
      $CEPH_BIN osd pool delete "$POOL_NAME" "$POOL_NAME" --yes-i-really-really-mean-it 2>>"$LOG_FILE" || true
      continue
    fi

    log "[2단계] Load 실행: num-tables=${NUM_TABLES}, write-top-pgs=${W}"
    if ! $BENCH_BIN --file "$BASE_FVECS" --pool "$POOL_NAME" \
      --num-tables "$NUM_TABLES" --write-top-pgs "$W" --pg-num "$PG_NUM" \
      2>>"$LOG_FILE" >>"$LOG_FILE"; then
      log "경고: Load 실패, 다음 케이스로 진행"
      $CEPH_BIN osd pool delete "$POOL_NAME" "$POOL_NAME" --yes-i-really-really-mean-it 2>>"$LOG_FILE" || true
      continue
    fi

    for P in "${PROBE_PGS_ARRAY[@]}"; do
      log "[3단계] Recall 실행: num-tables=${NUM_TABLES}, write-top-pgs=${W}, probe-pgs=${P}, query-num=${QUERY_NUM}"
      if ! $BENCH_BIN --recall --file "$BASE_FVECS" --pool "$POOL_NAME" \
        --query "$QUERY_FVECS" --gt "$GT_IVECS" \
        --num-tables "$NUM_TABLES" --pg-num "$PG_NUM" \
        --probe-mode "$PROBE_MODE" --probe-pgs "$P" --query-num "$QUERY_NUM" \
        2>>"$LOG_FILE" >>"$LOG_FILE"; then
        log "경고: Recall 실패 (write-top-pgs=${W}, probe-pgs=${P})"
      fi
    done

    log "[4단계] 풀 삭제: ${POOL_NAME}"
    if ! $CEPH_BIN osd pool delete "$POOL_NAME" "$POOL_NAME" --yes-i-really-really-mean-it 2>>"$LOG_FILE"; then
      log "경고: 풀 삭제 실패"
    fi
  done

  log "========== 벤치마크 종료 =========="
}

main 2>&1 | tee -a "$LOG_FILE"
exit 0
