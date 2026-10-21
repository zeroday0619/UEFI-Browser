/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UEFI_VENUS_DEVICE_H
#define UEFI_VENUS_DEVICE_H
#include "venus_transport.h"
int firmware_venus_device_open(struct firmware_venus_context **context,
                        struct firmware_venus_capabilities *capabilities);
void firmware_venus_device_close(struct firmware_venus_context *context);
int firmware_venus_device_submit(struct firmware_venus_context *context,
                          const void *command, size_t size, uint32_t timeline,
                          uint64_t *ticket);
int firmware_venus_device_poll(struct firmware_venus_context *context, uint64_t ticket);
void firmware_venus_device_release_fence(struct firmware_venus_context *context,
                                 uint64_t ticket);
int firmware_venus_device_create_blob(struct firmware_venus_context *context,
                               uint64_t memory_identifier, uint64_t size,
                               int mappable, struct firmware_venus_blob *blob);
void firmware_venus_device_destroy_blob(struct firmware_venus_context *context,
                                 struct firmware_venus_blob *blob);
void firmware_venus_device_present_image(const uint8_t *, uint32_t);
int firmware_venus_device_scanout(struct firmware_venus_context *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t);
int firmware_venus_device_retain_blob(struct firmware_venus_context *, uint32_t);
void firmware_venus_device_release_blob(uint32_t);
int firmware_venus_device_attach_blob(struct firmware_venus_context *, uint32_t);
void firmware_venus_device_detach_blob(struct firmware_venus_context *, uint32_t);
#endif
