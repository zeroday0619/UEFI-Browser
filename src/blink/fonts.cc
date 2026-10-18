// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <cstring>
#include <utility>
#include "base/check.h"
#include "base/check_op.h"
#include "skia/ext/font_utils.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkString.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/ports/SkFontMgr_data.h"

extern "C" {
extern const unsigned char noto_latin_start[], noto_latin_end[];
extern const unsigned char noto_cjk_start[], noto_cjk_end[];
}
namespace uefi_browser {
namespace {
class FirmwareFontManager final : public SkFontMgr {
 public:
  explicit FirmwareFontManager(sk_sp<SkFontMgr> manager)
      : manager_(std::move(manager)) {}
 private:
  const char* Family(const char* name) const {
    if (!name || !std::strcmp(name, "sans-serif") ||
        !std::strcmp(name, "serif") || !std::strcmp(name, "monospace") ||
        !std::strcmp(name, "system-ui") || !std::strcmp(name, "Arial"))
      return "Noto Sans";
    return name;
  }
  int onCountFamilies() const override { return manager_->countFamilies(); }
  void onGetFamilyName(int index, SkString* name) const override {
    manager_->getFamilyName(index, name);
  }
  sk_sp<SkFontStyleSet> onCreateStyleSet(int index) const override {
    return manager_->createStyleSet(index);
  }
  sk_sp<SkFontStyleSet> onMatchFamily(const char name[]) const override {
    return manager_->matchFamily(Family(name));
  }
  sk_sp<SkTypeface> onMatchFamilyStyle(const char name[],
                                      const SkFontStyle& style) const override {
    return manager_->matchFamilyStyle(Family(name), style);
  }
  sk_sp<SkTypeface> onMatchFamilyStyleCharacter(const char name[],
      const SkFontStyle& style, const char*[], int, SkUnichar character) const override {
    auto preferred = onMatchFamilyStyle(name, style);
    if (preferred && preferred->unicharToGlyph(character)) return preferred;
    // Embedded families provide fallback without filesystem or fontconfig access.
    for (int index = 0; index < manager_->countFamilies(); ++index) {
      auto family = manager_->createStyleSet(index);
      auto face = family->matchStyle(style);
      if (face && face->unicharToGlyph(character)) return face;
    }
    return nullptr;
  }
  sk_sp<SkTypeface> onMakeFromData(sk_sp<SkData> data, int index) const override {
    return manager_->makeFromData(std::move(data), index);
  }
  sk_sp<SkTypeface> onMakeFromStreamIndex(std::unique_ptr<SkStreamAsset> stream,
                                        int index) const override {
    return manager_->makeFromStream(std::move(stream), index);
  }
  sk_sp<SkTypeface> onMakeFromStreamArgs(std::unique_ptr<SkStreamAsset> stream,
                                        const SkFontArguments& arguments) const override {
    return manager_->makeFromStream(std::move(stream), arguments);
  }
  sk_sp<SkTypeface> onMakeFromFile(const char[], int) const override {
    return nullptr;
  }
  sk_sp<SkTypeface> onLegacyMakeTypeface(const char name[],
                                        SkFontStyle style) const override {
    return manager_->legacyMakeTypeface(Family(name), style);
  }
  sk_sp<SkFontMgr> manager_;
};
}
void InstallFonts() {
  sk_sp<SkData> data[] = {
      SkData::MakeWithoutCopy(noto_latin_start, noto_latin_end - noto_latin_start),
      SkData::MakeWithoutCopy(noto_cjk_start, noto_cjk_end - noto_cjk_start)};
  auto manager = SkFontMgr_New_Custom_Data(data);
  CHECK(manager);
  CHECK_GE(manager->countFamilies(), 2);
  skia::OverrideDefaultSkFontMgr(sk_make_sp<FirmwareFontManager>(std::move(manager)));
}
}
