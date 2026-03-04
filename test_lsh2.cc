#include <iostream>
#include <vector>
#include <stdint.h>
#include <iomanip>

extern "C" {
    #include "src/crush/hash.h"
}

/* LSH hash → PG (simplified: hash % pg_num; real Ceph uses stable_mod) */
uint32_t calculate_final_pg(uint32_t lsh_hash, uint32_t pg_num) {
    return lsh_hash % pg_num;
}

int main() {
    int dim = 1024;
    uint32_t pg_num = 128;

    std::vector<float> vec_A(dim, 0.5f);
    std::vector<float> vec_B(dim, 0.505f);
    std::vector<float> vec_C(dim, -0.5f);

    uint32_t hash_A = crush_hash32_lsh(vec_A.data(), dim);
    uint32_t hash_B = crush_hash32_lsh(vec_B.data(), dim);
    uint32_t hash_C = crush_hash32_lsh(vec_C.data(), dim);

    uint32_t pg_A = calculate_final_pg(hash_A, pg_num);
    uint32_t pg_B = calculate_final_pg(hash_B, pg_num);
    uint32_t pg_C = calculate_final_pg(hash_C, pg_num);

    std::cout << "========== PG Mapping Simulation ==========\n";
    std::cout << "Target PG Count: " << pg_num << "\n\n";

    std::cout << "Vector A -> Hash: " << std::setw(10) << hash_A << " -> FINAL PG: [" << pg_A << "]\n";
    std::cout << "Vector B -> Hash: " << std::setw(10) << hash_B << " -> FINAL PG: [" << pg_B << "] (Similar)\n";
    std::cout << "Vector C -> Hash: " << std::setw(10) << hash_C << " -> FINAL PG: [" << pg_C << "] (Different)\n";
    std::cout << "===========================================\n";

    if (pg_A == pg_B) {
        std::cout << "[SUCCESS] Two similar vectors were mapped to the same PG [" << pg_A << "]!\n";
        std::cout << "Result: These data points will be stored on the same set of OSDs (disks).\n";
    } else {
        std::cout << "[COLLISION FAIL] Hashes differed slightly, causing a mapping to different PGs.\n";
    }

    return 0;
}
