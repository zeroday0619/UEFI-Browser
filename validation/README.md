# Validation summaries and local artifacts

This directory publishes project-written Markdown reports describing implementation boundaries, test commands, observed results and unresolved issues. Historical results are observations from the recorded test runs, not claims that the tests were repeated for the current source revision.

## Publication boundary

Only top-level `*.md` files are eligible for source publication. All other files and subdirectories are ignored, including screenshots, framebuffer dumps, audio recordings, raw logs, JSON/JSONL results, text dumps, copied patches and checksum files. This default also excludes generated first-party fixtures and measurements. Do not force-add raw artifacts without a separate provenance and publication review.

External-site screenshots and media captures may contain third-party content. Raw diagnostics can contain fetched content, response metadata or development-host information. Their exclusion is a publication policy; it is not a determination of the legal status of each artifact. Required dependency notices in `licenses/` and the project license files remain part of the source distribution.

## Local evidence

Tests continue to write raw results into `validation/` for local debugging and comparison. Existing local files are retained when Git tracking is removed. Reports identify these files as local artifacts or show their names in code formatting. A clean checkout does not contain them, and the summaries alone do not independently reproduce the original observations. Use the documented test commands to generate new evidence.

Check the publication boundary with:

```sh
git ls-files validation
git status --short --ignored validation
```

The tracked-file list must contain only top-level Markdown reports. Review report prose before publication and avoid embedding website content or media in the reports themselves.

## Existing history

Ignore rules and tracking removal do not remove files from previous commits. Historical copies require a separately reviewed history cleanup or a clean source snapshot before publication. No history rewrite or remote deletion is performed by this change.
