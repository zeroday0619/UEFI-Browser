# AI-assisted development notice and disclaimer

## Development process

UEFI BROWSER is an experimental project developed with substantial AI assistance through OpenAI Codex. The workflow includes natural-language implementation requests, generated code and patches, tool-assisted debugging, build execution, tests and documentation. This iterative approach is sometimes called "vibe coding."

This disclosure concerns the project's development process. It does not attribute upstream Chromium, Blink, V8, Skia, musl, EDK II or other third-party code to AI. No percentage of AI-generated code or complete file-by-file authorship audit is claimed.

AI-generated changes can contain incorrect assumptions, incomplete implementations and defects that tests do not detect. Human review and validation are required before accepting AI-contributed changes. Publication of this repository does not establish that every change has received independent human review or a security audit.

## Verification limits

The evidence in [validation/](validation/) records specific builds, QEMU configurations and test scenarios. Passing a build, static check or website interaction test does not establish general correctness, browser security or compatibility with other sites and firmware.

Blink DOM, JavaScript, layout and Skia rasterization still execute in the main process. The isolated application-processor workers transfer completed frames; they do not provide a Chromium-equivalent renderer sandbox. Physical hardware has not been validated. Full renderer separation and broad browser compatibility remain incomplete. See the [README](README.md#project-status) for the current implementation boundary.

## Intended use and operational risk

The project is intended for experimentation, learning and firmware-porting research. It is not presented as a production-ready browser. Use a disposable QEMU environment with dedicated firmware variable files and test data. Do not rely on this build for sensitive accounts, confidential information or security-critical workflows.

Firmware-level defects can cause hangs, crashes, memory corruption or changes to persistent firmware state. QEMU results do not establish that execution on physical hardware is safe. The application operates through UEFI services and should not be treated as having the protections of a normal desktop browser process.

## Warranty and license scope

Original project contributions are distributed under the [BSD-3-Clause license](LICENSE), Copyright (c) 2026, zeroday0619. The software is provided "AS IS" under that license's warranty disclaimer and limitation of liability. The applicable license text governs those terms.

This notice is informational. It does not add restrictions to the BSD-3-Clause license, waive third-party obligations or replace any applicable license. Third-party software, fonts and certificate data retain their own terms; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). AI assistance does not establish third-party license compliance or eliminate the need to review provenance.

The project is an independent port. References to AI tools and upstream projects identify the tools and components used; they do not imply endorsement or certification.
