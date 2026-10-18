#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkGradient.h"
extern "C" {
#include "platform/runtime.h"
#include "uefi.h"
void graphics_log(const char *);
void platform_initialize(SystemTable *);
void platform_set_stack(void *, size_t);
int run_platform_probe(void);
void __cxa_finalize(void *);
void runtime_release_all(void);
void platform_release_pages(void);
#ifdef ENABLE_V8
int run_v8_probe(void);
#endif
}
struct Pixel {
  uint8_t blue, green, red, reserved;
};
struct ModeInfo {
  uint32_t version, width, height, format, masks[4], stride;
};
struct Mode {
  uint32_t maximum, current;
  ModeInfo *information;
  size_t size;
  uint64_t framebuffer;
  size_t framebuffer_size;
};
struct Graphics {
  void *query, *set;
  Status(UEFI_API *blit)(Graphics *, Pixel *, uint32_t, size_t, size_t, size_t,
                    size_t, size_t, size_t, size_t);
  Mode *mode;
};
static thread_local unsigned tls_probe = 7;
static Pixel pixels[640 * 480];
extern "C" void (*__init_array_start[])(void);
extern "C" void (*__init_array_end[])(void);
extern "C" int run_skia_probe(SystemTable *table) {
  if (tls_probe++ != 7 || tls_probe != 8) {
    graphics_log("FAIL: ELF TLS initialization\n");
    return 12;
  }
  graphics_log("PASS: native ELF thread-local storage\n");
  if (run_platform_probe()) {
    graphics_log("FAIL: platform validation\n");
    return 10;
  }
#ifdef ENABLE_V8
  if (run_v8_probe())
    return 11;
#endif
  graphics_log("ENGINE: native Skia raster begin\n");
  auto surface = SkSurfaces::WrapPixels(
      SkImageInfo::Make(640, 480, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
      pixels, 640 * 4);
  if (!surface) {
    graphics_log("FAIL: Skia surface\n");
    return 1;
  }
  SkCanvas *canvas = surface->getCanvas();
  canvas->clear(SkColorSetRGB(245, 247, 251));
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(SkColorSetRGB(20, 40, 72));
  canvas->drawRoundRect(SkRect::MakeXYWH(32, 32, 576, 120), 18, 18, paint);
  const SkPoint points[2] = {{50, 180}, {580, 430}};
  const SkColor4f colors[2] = {{0, 0, 1, 1}, {0, 1, 1, 1}};
  paint.setShader(SkShaders::LinearGradient(
      points,
      SkGradient(SkGradient::Colors({colors, 2}, {}, SkTileMode::kClamp), {})));
  canvas->drawCircle(185, 300, 110, paint);
  paint.setShader(nullptr);
  paint.setColor(SkColorSetARGB(190, 240, 80, 40));
  SkPathBuilder path;
  path.moveTo(360, 190)
      .cubicTo(620, 130, 300, 460, 580, 420)
      .lineTo(350, 420)
      .close();
  canvas->drawPath(path.detach(), paint);
  if (pixels[0].red != 245 || pixels[70 * 640 + 70].red != 20) {
    graphics_log("FAIL: Skia pixel validation\n");
    return 2;
  }
  const Pixel top = pixels[220 * 640 + 185], bottom = pixels[380 * 640 + 185],
              alpha = pixels[300 * 640 + 380];
  size_t partial = 0;
  for (int y = 190; y < 410; y++)
    for (int x = 75; x < 295; x++) {
      Pixel value = pixels[y * 640 + x];
      if (value.red > 0 && value.red < 245)
        partial++;
    }
  if (top.blue != 255 || top.red || bottom.green <= top.green ||
      alpha.red < 239 || alpha.red > 245 || alpha.green < 115 ||
      alpha.green > 130 || partial < 100) {
    graphics_log("FAIL: Skia gradient, alpha or edge validation\n");
    return 7;
  }
  graphics_log(
      "PASS: Skia CPU paths, curves, gradients, alpha and antialiasing\n");
  Guid guid = {0x9042a9de,
               0x23dc,
               0x4a38,
               {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
  Graphics *graphics = nullptr;
  Status result =
      ((Status(UEFI_API *)(Guid *, void *, void **))table->boot->function[37])(
          &guid, nullptr, (void **)&graphics);
  if (result || !graphics) {
    graphics_log("FAIL: GOP unavailable\n");
    return 3;
  }
  if (graphics->mode->information->width < 640 ||
      graphics->mode->information->height < 480)
    return 5;
  Pixel background = {251, 247, 245, 0};
  result = graphics->blit(graphics, &background, 0, 0, 0, 0, 0,
                          graphics->mode->information->width,
                          graphics->mode->information->height, 0);
  if (result)
    return 6;
  result = graphics->blit(graphics, pixels, 2, 0, 0, 0, 0, 640, 480, 640 * 4);
  if (result) {
    graphics_log("FAIL: GOP blit\n");
    return 4;
  }
  graphics_log("PASS: Skia framebuffer presented through UEFI GOP\n");
  return 0;
}

#include "include/private/SkLog.h"
#include <cstdarg>
#include <cstdio>
void SkLogVAList(SkLogPriority, const char *format, va_list arguments) {
  char buffer[512];
  vsnprintf(buffer, sizeof(buffer), format, arguments);
  graphics_log(buffer);
}
