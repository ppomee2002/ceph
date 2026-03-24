// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-

#include "ceph_vector_bench_io.h"

#include <fstream>
#include <iostream>

float* fvecs_read(const char* fname, size_t* d_out, size_t* n_out) {
  std::ifstream f(fname, std::ios::binary);
  if (!f) {
    std::cerr << "could not open " << fname << std::endl;
    return nullptr;
  }

  int d;
  f.read(reinterpret_cast<char*>(&d), sizeof(int));
  if (d <= 0 || d >= 1000000) {
    std::cerr << "unreasonable dimension: " << d << std::endl;
    return nullptr;
  }

  f.seekg(0, std::ios::end);
  size_t sz = f.tellg();
  f.seekg(0, std::ios::beg);

  size_t vec_bytes = (d + 1) * sizeof(float);
  if (sz % vec_bytes != 0) {
    std::cerr << "weird file size " << sz << " for dim " << d << std::endl;
    return nullptr;
  }
  size_t n = sz / vec_bytes;

  float* x = new float[n * d];
  for (size_t i = 0; i < n; i++) {
    int dummy;
    f.read(reinterpret_cast<char*>(&dummy), sizeof(int));
    f.read(reinterpret_cast<char*>(x + i * d), d * sizeof(float));
  }

  *d_out = d;
  *n_out = n;
  return x;
}

int* ivecs_read(const char* fname, size_t* k_out, size_t* n_out) {
  std::ifstream f(fname, std::ios::binary);
  if (!f) {
    std::cerr << "could not open " << fname << std::endl;
    return nullptr;
  }
  int k;
  f.read(reinterpret_cast<char*>(&k), sizeof(int));
  if (k <= 0 || k >= 100000) {
    std::cerr << "unreasonable k: " << k << std::endl;
    return nullptr;
  }
  f.seekg(0, std::ios::end);
  size_t sz = f.tellg();
  f.seekg(0, std::ios::beg);
  size_t row_bytes = (k + 1) * sizeof(int);
  if (sz % row_bytes != 0) {
    std::cerr << "weird ivecs size " << sz << std::endl;
    return nullptr;
  }
  size_t n = sz / row_bytes;
  int* gt = new int[n * k];
  for (size_t i = 0; i < n; i++) {
    int dummy;
    f.read(reinterpret_cast<char*>(&dummy), sizeof(int));
    f.read(reinterpret_cast<char*>(gt + i * k), k * sizeof(int));
  }
  *k_out = k;
  *n_out = n;
  return gt;
}
