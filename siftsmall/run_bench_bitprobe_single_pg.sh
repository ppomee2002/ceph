#!/bin/bash
set -euo pipefail

# 비트쉬프트(write_probe) 저장 + 쿼리당 PG 정확히 1개 (--probe 1 --read-l-tables 1)
# (write-mode 기본이 bitprobe이므로 명시 생략 가능; 여기서는 의도를 드러내기 위해 적음)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LSH_SIM="$REPO_ROOT/docs/lsh_offline_simulation.py"
LOG_FILE="$SCRIPT_DIR/bitprobe_single_pg_benchmark.log"

if [[ ! -f "$LSH_SIM" ]]; then
    echo "오류: $LSH_SIM 없음" >&2
    exit 1
fi

# ==========================================
TABLES=(1 3 5 16 32 64)

# 저장 시 인접 버킷 수 (multi_probe_pgs 상한). 1 = 복제 없음, 늘릴수록 쓰기 증폭·리콜 상승 여지
WRITE_PROBES=(1 2 4 8 16)

# 쿼리당 PG 1개: 테이블 0만 사용 + probe 1
READ_PROBE=1
READ_L_TABLES=1
# ==========================================

echo "==================================================" | tee "$LOG_FILE"
echo "LSH Bitprobe + 단일 PG 읽기 벤치마크 시작..." | tee -a "$LOG_FILE"
echo "시작 시간: $(date)" | tee -a "$LOG_FILE"
echo "==================================================" | tee -a "$LOG_FILE"

for t in "${TABLES[@]}"; do
    for wp in "${WRITE_PROBES[@]}"; do
        echo -e "\n[실험 진행 중] Tables: $t | write-probe: $wp | Read: ${READ_L_TABLES}table(s) probe $READ_PROBE"
        echo "==================================================" >> "$LOG_FILE"
        echo "[TEST CASE] mode: bitprobe | --l-tables $t --write-probe $wp --probe $READ_PROBE --read-l-tables $READ_L_TABLES" >> "$LOG_FILE"
        echo "==================================================" >> "$LOG_FILE"

        python3 "$LSH_SIM" \
            --write-mode bitprobe \
            --l-tables "$t" \
            --write-probe "$wp" \
            --probe "$READ_PROBE" \
            --read-l-tables "$READ_L_TABLES" \
            | tee -a "$LOG_FILE"
    done
done

echo -e "\n모든 테스트가 완료되었습니다! 결과는 $LOG_FILE 파일을 확인하세요."
