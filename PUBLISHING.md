# GitHub source publication

## License scope

Original contributions use BSD-3-Clause, Copyright (c) 2026, zeroday0619. Include `LICENSE`, `THIRD_PARTY_NOTICES.md`, the complete `licenses/` directory, `revisions.json` and `fonts/manifest.json` in source releases. Preserve existing file-level copyright and license notices.

The current source tree can be prepared independently of binary release licensing. The EFI executable incorporates a larger third-party graph whose notices and applicable source/relinking obligations still require a release-specific inventory. Do not label the development EFI archive as a license-audited release.

## Repository contents

Publish source, build scripts, patches, configuration, bundled fonts and certificate data, license notices, tests and validation documentation. The `build/` directory, downloaded upstream repositories, generated EFI/ELF files and firmware variable profiles are excluded. Bundled font binaries are intentional source assets and retain their OFL notices.

The build scripts currently resolve the working directory two levels above the repository. Use this layout:

```text
workspace/
  outputs/
    chromium-uefi/     # Git checkout or extracted source archive
  work/               # Downloaded upstream sources and build intermediates
```

Running from an arbitrary checkout path has not been validated. See the README for host tools and build commands.

## Review before pushing

1. Review `git status --short` and the files selected for publication. Confirm that no credentials, private test data, firmware profiles or generated executables are included.
2. Review existing upstream copyright headers, patch provenance and third-party notice coverage. File-level terms remain applicable even when GitHub identifies the repository's root license as BSD-3-Clause.
3. Review any staged diff before creating the first commit. Follow the project's commit-message and human-review requirements.
4. Configure the intended GitHub repository and visibility, then push the reviewed source commit. No remote URL or repository visibility is assumed by these documents.

Validation logs contain historical development-host paths and website URLs. These are retained as test evidence and are not portable build instructions. Website screenshots demonstrate compatibility and do not grant rights to third-party website content or marks.

No build or QEMU rerun is required to validate documentation-only licensing changes. Existing execution evidence remains in `validation/`; a source release does not imply that every documented feature or dependency was revalidated for publication.
