#include <iostream>
#include <vector>
#include <stdint.h>

extern "C" {
    #include "src/crush/hash.h"
}

int main() {
    int dim = 1024;
    std::vector<float> vec_A(dim, 0.5f);
    std::vector<float> vec_B(dim, 0.501f);  /* similar */
    std::vector<float> vec_C(dim, -0.5f);   /* dissimilar */

    uint32_t hash_A = crush_hash32_lsh(vec_A.data(), dim);
    uint32_t hash_B = crush_hash32_lsh(vec_B.data(), dim);
    uint32_t hash_C = crush_hash32_lsh(vec_C.data(), dim);

    std::cout << "========== LSH Prototype Test ==========\n";
    std::cout << "Vector A Hash : " << hash_A << "\n";
    std::cout << "Vector B Hash : " << hash_B << " (Similar to A)\n";
    std::cout << "Vector C Hash : " << hash_C << " (Different from A)\n";
    std::cout << "========================================\n";

    if (hash_A == hash_B) {
        std::cout << "[SUCCESS] Similar vectors mapped to the same 32-bit bucket (PG)!\n";
    } else {
        std::cout << "[FAILURE] Similar vectors produced different hashes. (Boundary issue or bug)\n";
    }

    if (hash_A != hash_C) {
        std::cout << "[SUCCESS] Dissimilar vectors produced different hash values!\n";
    } else {
        std::cout << "[FAILURE] Hash collision occurred between dissimilar vectors.\n";
    }

    return 0;
}
