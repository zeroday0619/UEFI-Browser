/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UEFI_VENUS_IMAGE_H
#define UEFI_VENUS_IMAGE_H
#include <stdint.h>
/* The broker accepts only immutable, single-plane, single-mip color images.
 * Alpha bit 0 enables alpha, bit 1 flips the origin, and bit 2 selects straight
 * alpha. Bits 3 through 5 encode Blink's bounded PredefinedColorSpace value.
 * No renderer virtual address or Vulkan object handle crosses this boundary. */
struct firmware_venus_image {
    uint32_t width, height, format, alpha;
    uint64_t size;
    uint32_t memory_type, resource;
};
#endif
