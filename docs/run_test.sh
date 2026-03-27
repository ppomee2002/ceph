# Initialize the results log file
echo "LSH Parameter Automation Test Results" > auto_test_results.log

# Automatically run tests by varying tables (t) from 3 to 8 and probes (p) through 1, 3, 5
for t in 32 48 64 96 128; do
  for p in 2 4 8 16 32; do
    echo "==================================================" | tee -a auto_test_results.log
    echo "[RUNNING] Tables: $t / Probes: $p" | tee -a auto_test_results.log
    echo "==================================================" | tee -a auto_test_results.log
    
    python3 /home/dev_path/ceph/docs/lsh_offline_simulation.py \
      --base /home/dev_path/ceph/sift1M/sift/sift_base.fvecs \
      --query /home/dev_path/ceph/sift1M/sift/sift_query.fvecs \
      --gt /home/dev_path/ceph/sift1M/sift/sift_groundtruth.ivecs \
      --bits 32 --disable-pg --l-tables $t -Q 100 --probe $p \
      -j $(nproc) --use-processes >> auto_test_results.log 2>&1
      
  done
done

echo "All tests have been completed. Please check the auto_test_results.log file."
