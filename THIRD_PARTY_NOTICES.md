# Third-party licensing notes

This document records the current licensing inventory for publication preparation. It is not a complete notice bundle for the linked EFI executable. Original project contributions are licensed under [BSD-3-Clause](LICENSE), Copyright (c) 2026, zeroday0619. This license does not replace third-party licenses or upstream copyright notices. File-specific terms take precedence for the material they cover.

## Existing notices

| Component | License scope | Local notice |
| --- | --- | --- |
| Chromium | BSD-3-Clause for code covered by its top-level license; dependencies and file-specific notices differ | [Chromium](licenses/chromium.txt) |
| Blink | File-specific terms, including BSD and GNU Library General Public License version 2 or later | [LGPL v2 text](licenses/LGPL-2.0.txt); upstream file headers determine scope; full binary inventory pending |
| V8 | BSD-style license and separately licensed external components | [V8](licenses/v8.txt) |
| Skia and skcms | BSD-style licenses; linked dependencies retain their own terms | [Skia](licenses/skia.txt), [skcms](licenses/skcms.txt) |
| libvpx | BSD-style license and separate patent grant for the bundled VP8/VP9 decoder; additional third-party files retain their own terms | [License](licenses/libvpx-LICENSE), [patent grant](licenses/libvpx-PATENTS) |
| libyuv | BSD-style license and separate patent grant for pixel conversion | [License](licenses/libyuv-LICENSE), [patent grant](licenses/libyuv-PATENTS) |
| musl 1.2.5 | MIT overall, with exceptions identified in COPYRIGHT | [musl](licenses/musl-COPYRIGHT) |
| LLVM libc++, libc++abi and libc | Apache-2.0 with LLVM exceptions; preserve complete bundled notices | [libc++](licenses/libc++-LICENSE.TXT), [libc++abi](licenses/libc++abi-LICENSE.TXT), [libc](licenses/llvm-libc.txt) |
| EDK II | BSD-2-Clause-Patent for covered code; package-specific exceptions require inventory | [EDK II](licenses/EDK-II.txt) |
| Noto fonts | SIL Open Font License 1.1 | [Noto](licenses/Noto-LICENSE), [Noto CJK](licenses/Noto-CJK-LICENSE) |
| Mozilla-derived certificate bundle | MPL-2.0 | [MPL](licenses/MPL-2.0.txt), [bundle](trust/mozilla-ca.pem) |
| GNU runtime notices retained from earlier configurations | File-specific GCC terms and applicable runtime exceptions; presence of these notices does not establish linkage in the current image | [Debian GCC notice](licenses/libstdc++.txt), [GPLv3](licenses/GPL-3.txt) |

Pinned source revisions are recorded in [revisions.json](revisions.json). For example, the pinned Blink file `third_party/blink/renderer/platform/transforms/matrix_transform_operation.cc` carries a GNU Library General Public License version 2-or-later header. Chromium's root BSD license is therefore not a complete description of Blink licensing.

The certificate bundle identifies its Mozilla extraction date and source in its header. curl documents that the converted PEM retains MPL-2.0 terms: [CA extract licensing](https://curl.se/docs/caextract.html#license). This is a static certificate bundle, not Firefox's full certificate policy implementation.

## Before a binary release

1. Inventory the actual linked inputs, embedded drivers, fonts, resources and generated code for the release build. Record revisions, license identifiers, copyright notices and local modifications. Chromium's [license tooling](https://github.com/chromium/chromium/tree/main/tools/licenses) can assist, but must be checked against this custom static link.
2. Include all required license texts and notices. The files currently in `licenses/` do not cover the complete dependency graph.
3. Identify the applicable license version and distribution obligations for each LGPL-covered input. For a static executable, review corresponding source and relinking requirements under the applicable license; a link to the top-level Chromium repository alone does not establish compliance.
4. Preserve source and notices needed for MPL-covered data and any modified covered files. Mozilla provides [MPL distribution guidance](https://www.mozilla.org/en-US/MPL/2.0/FAQ/).
5. Review provenance of copied files and patches before adding project copyright headers. Do not replace upstream authorship notices or assume that a file carrying an upstream header is original project code.
6. Keep guest firmware profiles, generated binaries, private test data and build caches out of source commits. Supply binary releases separately with the notices and source materials required for those artifacts.

## Scope of the project license

The root BSD-3-Clause license applies to original project code and documentation except where another license is identified. Third-party files, fonts, certificate data and upstream-derived portions of patches retain their existing terms. Original modifications do not remove obligations attached to the modified upstream code. Existing copyright headers have been preserved rather than replaced with the project owner's name.

Upstream sources and build inputs are pinned in `revisions.json`. Font origins and hashes are recorded in `fonts/manifest.json`. The local GNU license copies are unmodified texts from the host's `/usr/share/common-licenses/LGPL-2` and `LGPL-2.1`; canonical texts are available from the [GNU license archive](https://www.gnu.org/licenses/old-licenses/).

The source publication archive excludes downloaded upstream repositories, generated EFI binaries and firmware variable profiles. A binary release requires the additional inventory described above. Historical validation images and screenshots describe observed behavior; website content and third-party marks shown in screenshots are not relicensed by the project license.
