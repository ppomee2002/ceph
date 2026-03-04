#include <iostream>
#include <vector>
#include <stdint.h>
#include <cmath>
#include <random>
#include <iomanip>

extern "C" {
    #include "src/crush/hash.h"
}

/* cosine similarity - unused, kept for debug */
float get_cosine_sim(const std::vector<float>& a, const std::vector<float>& b) {
    float dot = 0.0, n_a = 0.0, n_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        n_a += a[i] * a[i];
        n_b += b[i] * b[i];
    }
    return dot / (std::sqrt(n_a) * std::sqrt(n_b));
}

int main() {
    int dim = 1024;
    int iterations = 1000;
    uint32_t pg_num = 128;
    int success_count = 0;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(-1.0, 1.0);

    std::cout << "Starting Recall Analysis (Iterations: " << iterations << ")..." << std::endl;

    for (int i = 0; i < iterations; ++i) {
        std::vector<float> vec_A(dim);
        for(int j=0; j<dim; ++j) vec_A[j] = dis(gen);

        /* B = A + small noise */
        std::vector<float> vec_B(dim);
        for(int j=0; j<dim; ++j) vec_B[j] = vec_A[j] + (dis(gen) * 0.05f);

        uint32_t pg_A = crush_hash32_lsh(vec_A.data(), dim) % pg_num;
        uint32_t pg_B = crush_hash32_lsh(vec_B.data(), dim) % pg_num;

        if (pg_A == pg_B)
            success_count++;
    }

    double recall = (double)success_count / iterations * 100.0;

    std::cout << "\n==========================================" << std::endl;
    std::cout << "   LSH PG Co-location Recall Result       " << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "Total Pairs      : " << iterations << std::endl;
    std::cout << "Matching PGs     : " << success_count << std::endl;
    std::cout << "Recall (Accuracy): " << std::fixed << std::setprecision(2) << recall << "%" << std::endl;
    std::cout << "==========================================" << std::endl;

    return 0;
}
