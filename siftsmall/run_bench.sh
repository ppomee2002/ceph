#!/bin/bash
set -euo pipefail

# 경로 설정 (현재 스크립트 위치 기준)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LSH_SIM="$REPO_ROOT/docs/lsh_offline_simulation.py"
LOG_FILE="$SCRIPT_DIR/shadow_mode_benchmark.log"

if [[ ! -f "$LSH_SIM" ]]; then
    echo "오류: $LSH_SIM 없음" >&2
    exit 1
fi

# ==========================================
# 테스트할 변수들 조합
# ==========================================
# 1. 테이블 수
TABLES=(1 3 5 16 32 64 128 256)

# 2. 임계값(shadow-margin) 설정
# 0.0: 중복 저장 아예 안 함 (가장 작은 디스크 사용량)
# 0.1: 아주 타이트한 경계만 2순위 저장
# 0.5: 중간 정도의 마진 허용
# 1.0: 꽤 넉넉한 마진 허용 (중복 저장 증가)
# inf: 조건 없이 무조건 가장 약한 축을 기준으로 1개씩 중복 저장 (최대 디스크 사용량)
MARGINS=(0.0 1.0 inf)

# 3. 쿼리 시 읽기: --probe 1만으로는 테이블마다 PG 1개씩 합쳐져 최대 (테이블 수)개 PG가 됨.
#    쿼리당 PG를 정확히 1개만 열려면 --read-l-tables 1 (테이블 0만 사용).
READ_PROBE=1
READ_L_TABLES=1
# ==========================================

echo "==================================================" | tee "$LOG_FILE"
echo "LSH Shadow 모드 (임계값 기반) 벤치마크 시작..." | tee -a "$LOG_FILE"
echo "시작 시간: $(date)" | tee -a "$LOG_FILE"
echo "==================================================" | tee -a "$LOG_FILE"

for t in "${TABLES[@]}"; do
    for margin in "${MARGINS[@]}"; do
        echo -e "\n[실험 진행 중] Tables: $t | Shadow-Margin: $margin | Read: ${READ_L_TABLES}table(s) probe $READ_PROBE"
        echo "==================================================" >> "$LOG_FILE"
        echo "[TEST CASE] mode: shadow | --l-tables $t --shadow-margin $margin --probe $READ_PROBE --read-l-tables $READ_L_TABLES" >> "$LOG_FILE"
        echo "==================================================" >> "$LOG_FILE"

        python3 "$LSH_SIM" \
            --write-mode shadow \
            --l-tables "$t" \
            --shadow-margin "$margin" \
            --probe "$READ_PROBE" \
            --read-l-tables "$READ_L_TABLES" \
            | tee -a "$LOG_FILE"
    done
done

echo -e "\n모든 테스트가 완료되었습니다! 결과는 $LOG_FILE 파일을 확인하세요."