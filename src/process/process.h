#ifndef UEFI_RENDER_PROCESSES_H
#define UEFI_RENDER_PROCESSES_H
#include "uefi.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  size_t workers, frames, failures, starts, idle_stops, fallback_frames;
  int running, stopping, disabled;
  size_t processor[4];
} RenderProcessSnapshot;
void render_processes_snapshot(RenderProcessSnapshot *snapshot);
int render_processes_initialize(SystemTable *table, size_t maximum_pixels);
int render_processes_present(const void *pixels, void *framebuffer,
                             size_t width, size_t height, size_t stride,
                             int red_first);
void render_processes_idle(void);
void render_processes_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
