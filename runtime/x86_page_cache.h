/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UEFI_X86_PAGE_CACHE_H
#define UEFI_X86_PAGE_CACHE_H
#include <stdint.h>

/* Host write-combined mappings must never acquire a guest write-back alias.
 * An uncached PAT entry is a conservative fallback when firmware has no WC slot. */
static inline int x86_device_page_cache(uint64_t pat, unsigned cache_type,
                                        uint64_t *flags)
{
    if (cache_type < 1 || cache_type > 3) return 0;
    unsigned wanted = cache_type == 1 ? 6 : cache_type == 3 ? 1 : 0;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        for (unsigned index = 0; index < 8; ++index) {
            if (((pat >> (index * 8)) & 255) != wanted) continue;
            *flags = ((uint64_t)(index & 3) << 3) | ((uint64_t)(index & 4) << 5);
            return 1;
        }
        if (wanted != 1) break;
        wanted = 0;
    }
    return 0;
}
#endif
