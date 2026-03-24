// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
#pragma once

#include <cstddef>

/** .fvecs: [4B dim][dim floats] per row. Caller frees with delete[]. */
float* fvecs_read(const char* fname, size_t* d_out, size_t* n_out);

/** .ivecs ground truth: each row = k NN indices. Caller frees with delete[]. */
int* ivecs_read(const char* fname, size_t* k_out, size_t* n_out);
