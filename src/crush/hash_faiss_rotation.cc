// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
#include "crush_compat.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

#ifndef __KERNEL__

#ifdef CEPH_CRUSH_HAVE_LAPACK_QR
extern "C" {
int sgeqrf_(int* m, int* n, float* a, int* lda, float* tau, float* work, int* lwork, int* info);
int sorgqr_(int* m, int* n, int* k, float* a, int* lda, float* tau, float* work, int* lwork, int* info);
}
#endif

namespace {

constexpr int kMaxDim = 1024;
constexpr int kMaxTables = 256;

int normalize_dim(int dim) {
  if (dim <= 0) {
    return 0;
  }
  if (dim > kMaxDim) {
    return kMaxDim;
  }
  return dim;
}

int normalize_table_id(int table_id) {
  int t = table_id % kMaxTables;
  if (t < 0) {
    t += kMaxTables;
  }
  return t;
}

struct FaissRandomGenerator {
  explicit FaissRandomGenerator(int64_t seed) : mt(static_cast<unsigned int>(seed)) {}

  int rand_int() {
    return static_cast<int>(mt() & 0x7fffffffU);
  }

  double rand_double() {
    return mt() / static_cast<double>(mt.max());
  }

  std::mt19937 mt;
};

void float_randn_faiss_compat(float* x, size_t n, int64_t seed) {
  const size_t nblock = (n < 1024) ? 1 : 1024;
  FaissRandomGenerator rng0(seed);
  const int a0 = rng0.rand_int();
  const int b0 = rng0.rand_int();

  for (size_t j = 0; j < nblock; ++j) {
    FaissRandomGenerator rng(static_cast<int64_t>(a0) + static_cast<int64_t>(j) * b0);
    double a = 0.0;
    double b = 0.0;
    double s = 0.0;
    int state = 0;
    const size_t istart = j * n / nblock;
    const size_t iend = (j + 1) * n / nblock;

    for (size_t i = istart; i < iend; ++i) {
      if (state == 0) {
        do {
          a = 2.0 * rng.rand_double() - 1.0;
          b = 2.0 * rng.rand_double() - 1.0;
          s = a * a + b * b;
        } while (s >= 1.0);
        x[i] = static_cast<float>(a * std::sqrt(-2.0 * std::log(s) / s));
      } else {
        x[i] = static_cast<float>(b * std::sqrt(-2.0 * std::log(s) / s));
      }
      state = 1 - state;
    }
  }
}

/*
 * Faiss matrix_qr() equivalent fallback that orthonormalizes columns in-place
 * for a column-major matrix (m rows x n cols, m >= n).
 */
void matrix_qr_fallback(int m, int n, float* a) {
  const float eps = 1e-12f;
  for (int col = 0; col < n; ++col) {
    float* v = a + static_cast<size_t>(col) * m;
    for (int prev = 0; prev < col; ++prev) {
      const float* q = a + static_cast<size_t>(prev) * m;
      float proj = 0.0f;
      for (int i = 0; i < m; ++i) {
        proj += v[i] * q[i];
      }
      for (int i = 0; i < m; ++i) {
        v[i] -= proj * q[i];
      }
    }

    float norm2 = 0.0f;
    for (int i = 0; i < m; ++i) {
      norm2 += v[i] * v[i];
    }
    if (norm2 <= eps) {
      for (int i = 0; i < m; ++i) {
        v[i] = 0.0f;
      }
      v[col % m] = 1.0f;
      norm2 = 1.0f;
    }
    const float inv = 1.0f / std::sqrt(norm2);
    for (int i = 0; i < m; ++i) {
      v[i] *= inv;
    }
  }
}

void matrix_qr_faiss_compat(int m, int n, float* a) {
#ifdef CEPH_CRUSH_HAVE_LAPACK_QR
  int ki = (m < n) ? m : n;
  std::vector<float> tau(static_cast<size_t>(ki));
  int lwork = -1;
  int info = 0;
  float work_size = 0.0f;

  sgeqrf_(&m, &n, a, &m, tau.data(), &work_size, &lwork, &info);
  if (info != 0) {
    matrix_qr_fallback(m, n, a);
    return;
  }
  lwork = static_cast<int>(work_size);
  if (lwork < 1) {
    lwork = 1;
  }
  std::vector<float> work(static_cast<size_t>(lwork));
  sgeqrf_(&m, &n, a, &m, tau.data(), work.data(), &lwork, &info);
  if (info != 0) {
    matrix_qr_fallback(m, n, a);
    return;
  }
  sorgqr_(&m, &n, &ki, a, &m, tau.data(), work.data(), &lwork, &info);
  if (info != 0) {
    matrix_qr_fallback(m, n, a);
  }
#else
  matrix_qr_fallback(m, n, a);
#endif
}

struct RotationKey {
  int dim = 0;
  int table_id = 0;
  __u32 rot_seed = 0;
};

struct RotationKeyHash {
  size_t operator()(const RotationKey& k) const {
    uint64_t h = static_cast<uint64_t>(k.rot_seed);
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(k.table_id)) * 0x9e3779b97f4a7c15ULL;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(k.dim)) * 0xbf58476d1ce4e5b9ULL;
    return static_cast<size_t>(h);
  }
};

struct RotationKeyEq {
  bool operator()(const RotationKey& a, const RotationKey& b) const {
    return a.dim == b.dim && a.table_id == b.table_id && a.rot_seed == b.rot_seed;
  }
};

const std::vector<float>& get_rotation_matrix_faiss_compat(int dim, int table_id, __u32 rot_seed) {
  static std::mutex mtx;
  static std::unordered_map<RotationKey, std::vector<float>, RotationKeyHash, RotationKeyEq> cache;

  const RotationKey key{dim, table_id, rot_seed};
  std::lock_guard<std::mutex> g(mtx);
  auto it = cache.find(key);
  if (it != cache.end()) {
    return it->second;
  }

  std::vector<float> mat(static_cast<size_t>(dim) * dim);
  const uint32_t faiss_seed = static_cast<uint32_t>(rot_seed) +
                              static_cast<uint32_t>(table_id) * 99991u;
  float_randn_faiss_compat(mat.data(), mat.size(), static_cast<int64_t>(faiss_seed));
  matrix_qr_faiss_compat(dim, dim, mat.data());
  auto inserted = cache.emplace(key, std::move(mat));
  return inserted.first->second;
}

}  // namespace

extern "C" int crush_hash32_faiss_rotation_apply_ref(const float* vector, int dim, int table_id,
                                                      __u32 rot_seed, float* out_rotated,
                                                      int out_dim) {
  dim = normalize_dim(dim);
  if (!vector || !out_rotated || dim <= 0 || out_dim < dim) {
    return 0;
  }
  table_id = normalize_table_id(table_id);

  const auto& mat = get_rotation_matrix_faiss_compat(dim, table_id, rot_seed);
  for (int j = 0; j < dim; ++j) {
    const float* col = mat.data() + static_cast<size_t>(j) * dim;
    float dot = 0.0f;
    for (int i = 0; i < dim; ++i) {
      dot += col[i] * vector[i];
    }
    out_rotated[j] = dot;
  }
  return dim;
}

#endif  // !__KERNEL__
