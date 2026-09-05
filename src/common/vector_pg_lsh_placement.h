// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#ifndef CEPH_COMMON_VECTOR_PG_LSH_PLACEMENT_H
#define CEPH_COMMON_VECTOR_PG_LSH_PLACEMENT_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

// pg-lsh-v0 sub_oid primitives shared by two call sites:
//   - librados/vector_placement.h, client-side put/query routing
//   - the crimson OSD's boundary-aware query expansion over the same
//     sub_oid key space (vector_pg_lsh_boundary.h)
// Both have to derive bit-identical hyperplane signs and sub_oid strings
// from the same inputs, so the logic lives here once rather than on both
// sides. librados/vector_placement.h re-exports these under
// librados::vector_placement for existing callers.
namespace ceph::rados::vector_pg_lsh_placement {

inline uint32_t mix_u32(uint32_t value)
{
  value ^= value >> 16;
  value *= 0x7feb352dU;
  value ^= value >> 15;
  value *= 0x846ca68bU;
  value ^= value >> 16;
  return value;
}

// Sign of the (table, bit, dimension) hyperplane, used for pg-lsh-v0
// PG-selection groups and, with a residual-specific seed, for
// compute_sub_oid()'s anchor-orthogonal residual hyperplanes.
inline int pg_lsh_v0_hyperplane_sign(uint32_t seed,
                                     uint32_t table,
                                     uint32_t bit,
                                     uint32_t dimension)
{
  const uint32_t mixed_seed =
    seed ^ table * 0x9e3779b9U ^ bit * 0x85ebca6bU ^
    dimension * 0xc2b2ae35U;
  return (mix_u32(mixed_seed) & 1U) == 0U ? -1 : 1;
}

inline std::string hex_u32_width(uint32_t value, uint32_t width)
{
  char buf[9];
  if (width == 0) {
    width = 1;
  }
  if (width > 8) {
    width = 8;
  }
  std::snprintf(buf, sizeof(buf), "%0*x", static_cast<int>(width), value);
  return std::string(buf);
}

inline uint32_t sub_oid_hex_width(uint32_t bits)
{
  return std::max<uint32_t>(1, (bits + 3) / 4);
}

struct sub_oid_t {
  uint16_t distance_bucket = 0;
  uint16_t residual_code = 0;
};

inline bool sub_oid_enabled(uint32_t distance_bucket_bits,
                            uint32_t residual_bits)
{
  return distance_bucket_bits != 0 || residual_bits != 0;
}

// "gXXXX_rYYYY" -- must stay in sync with parse_sub_oid() below.
inline std::string format_sub_oid(const sub_oid_t& sub_oid,
                                  uint32_t distance_bucket_bits,
                                  uint32_t residual_bits)
{
  const uint32_t distance_width = sub_oid_hex_width(distance_bucket_bits);
  const uint32_t residual_width = sub_oid_hex_width(residual_bits);
  return "g" + hex_u32_width(sub_oid.distance_bucket, distance_width) +
    "_r" + hex_u32_width(sub_oid.residual_code, residual_width);
}

// Inverse of format_sub_oid(). Returns nullopt for anything that is not
// exactly "g<distance_width hex>_r<residual_width hex>" at the given bit
// widths, such as a name from another placement scheme or a foreign
// sibling under the same prefix.
inline std::optional<sub_oid_t> parse_sub_oid(std::string_view formatted,
                                              uint32_t distance_bucket_bits,
                                              uint32_t residual_bits)
{
  const uint32_t distance_width = sub_oid_hex_width(distance_bucket_bits);
  const uint32_t residual_width = sub_oid_hex_width(residual_bits);
  const size_t expected_len = 1 + distance_width + 2 + residual_width;
  if (formatted.size() != expected_len || formatted[0] != 'g') {
    return std::nullopt;
  }
  const std::string_view distance_hex = formatted.substr(1, distance_width);
  const std::string_view separator =
    formatted.substr(1 + distance_width, 2);
  const std::string_view residual_hex =
    formatted.substr(1 + distance_width + 2, residual_width);
  if (separator != "_r") {
    return std::nullopt;
  }

  auto parse_hex = [](std::string_view hex, uint32_t *out) {
    uint32_t value = 0;
    for (const char c : hex) {
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else {
        return false;
      }
    }
    *out = value;
    return true;
  };

  uint32_t distance_bucket = 0;
  uint32_t residual_code = 0;
  if (!parse_hex(distance_hex, &distance_bucket) ||
      !parse_hex(residual_hex, &residual_code)) {
    return std::nullopt;
  }
  return sub_oid_t{
    static_cast<uint16_t>(distance_bucket),
    static_cast<uint16_t>(residual_code),
  };
}

} // namespace ceph::rados::vector_pg_lsh_placement

#endif
