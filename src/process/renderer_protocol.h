#ifndef UEFI_RENDERER_PROTOCOL_H
#define UEFI_RENDERER_PROTOCOL_H
#include <stdint.h>
/* Messages contain values only so transport does not depend on Blink pointers. */
typedef struct {
  uint32_t version, kind;
  uint64_t generation, sequence;
  int32_t x, y, wheel;
  uint32_t buttons;
  uint16_t scan, character;
  uint32_t shift;
  uint8_t toggle;
  uint8_t wheel_consumed;
  uint8_t reserved[6];
} RendererInputMessage;
#ifdef __cplusplus
static_assert(sizeof(RendererInputMessage) == 56, "Renderer input wire layout");
#else
_Static_assert(sizeof(RendererInputMessage) == 56, "Renderer input wire layout");
#endif
#define RENDERER_PROTOCOL_VERSION 1u
#define RENDERER_INPUT_POINTER 1u
#define RENDERER_INPUT_KEY 2u
#define RENDERER_INPUT_CAPACITY 256u
#endif
