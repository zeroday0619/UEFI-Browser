# Fluent-style browser settings

Validated on 2026-10-18 with QEMU 11.1.2, KVM, OVMF 2026.08, 4 GiB guest RAM and a 1280 × 800 framebuffer. The EFI image hash is recorded in `fluent-result.json` (local artifact). No production dependency was added.

## Interface and behavior

The native Skia toolbar has rounded navigation controls, a theme-aware address field and a settings gear. Ctrl+, opens settings; Ctrl+L retains address entry. Blink renders local `about:settings` and `about:version` documents with a sidebar, searchable settings categories, selected choices, keyboard focus states and light/dark surfaces. The design follows Fluent 2's [neutral and brand color roles](https://fluent2.microsoft.design/color) and [typographic hierarchy](https://fluent2.microsoft.design/typography), using the existing Noto fonts rather than bundling Segoe or a UI framework.

Implemented preferences are browser theme, default website font size, JavaScript, images and startup page. Site controls affect subsequent navigations and reloads. Explicit site CSS overrides the default font size. Internal settings retain JavaScript for search and keyboard interaction. Reset uses an explicit confirmation. Browser history remains session-only.

About displays Chromium version and revision generated from the verified checkout, V8's version macro, Skia software rendering and the musl platform. It identifies the absence of an automatic updater and unsupported Chrome services without presenting nonfunctional controls.

A bounded, versioned preference record is saved through UEFI GetVariable/SetVariable under a dedicated vendor GUID. Invalid records fall back to defaults. Save failure is reported on the settings page. The runner retains a per-build firmware variable file and locks it against concurrent use; tests can deliberately reuse their separate test variable file.

Settings mutations require a current internal page, a matching requestor origin and transient user activation. Arbitrary query strings are rejected by an exact command allowlist. Ordinary websites cannot mutate preferences. HTTP redirects remain restricted to credential-free HTTP(S) URLs.

## Validation

All four device-input scenarios passed on the final EFI image and returned cleanly to firmware. Tests use QMP keyboard/mouse events and framebuffer pixels, not DOM injection.

| Scenario | Evidence |
| --- | --- |
| `settings` | Gear navigation; light/dark pixels; font, script and image choices; category search; startup selection; About and back |
| `settings-persist` | A second QEMU process restores the same profile; JavaScript-disabled red panel and hidden image; external setting link rejected; enabling script and image produces green panels and computed 20px font |
| `settings-reset` | Ctrl+,; keyboard search and confirmation; all defaults restored; About/back/forward |
| `history` | Existing website back, forward and reload behavior without duplicate reload entries |

Reproduction from the workspace root:

```sh
python3 outputs/chromium-uefi/build-blink-libraries.py --resolve-inputs --jobs 20
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/outputs/chromium-uefi/build/blink" outputs/chromium-uefi/build-musl.sh
python3 outputs/chromium-uefi/tests/redirect-server.py
# Run the following commands in another terminal, sequentially.
QEMU_TEST_ACCEL=kvm INPUT_SCENARIO=settings python3 outputs/chromium-uefi/tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_KEEP_VARIABLES=1 INPUT_SCENARIO=settings-persist python3 outputs/chromium-uefi/tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm QEMU_TEST_KEEP_VARIABLES=1 INPUT_SCENARIO=settings-reset python3 outputs/chromium-uefi/tests/browser-qemu.py
QEMU_TEST_ACCEL=kvm INPUT_SCENARIO=history python3 outputs/chromium-uefi/tests/browser-qemu.py
BUILD_DIRECTORY="$PWD/outputs/chromium-uefi/build/blink" python3 outputs/chromium-uefi/validate-binary.py
```

The static ELF/EFI validation passed: no required unresolved symbols, Linux syscall instructions or PE imports. Shell syntax and Python compilation checks also passed. Logs: settings (`fluent-settings.log`; local artifact), persistence and website behavior (`fluent-settings-persist.log`; local artifact), reset and internal history (`fluent-settings-reset.log`; local artifact), website history (`fluent-history.log`; local artifact), binary (`fluent-binary.log`; local artifact).

Screenshots were visually inspected: light settings (`fluent-settings-light.png`; local artifact), dark settings (`fluent-settings-dark.png`; local artifact), About (`fluent-about.png`; local artifact) and search (`fluent-search.png`; local artifact).

## Changed files and limits

- `src/blink/browser_settings.h`: internal page markup, preference validation and firmware persistence.
- `src/blink/browser_entry.cc`: Fluent-inspired toolbar, settings entry, keyboard shortcut and startup preference.
- `src/blink/browser_frame.cc`: engine settings and trusted internal commands.
- `src/blink/navigation.cc`: local documents and redirect scheme checks.
- `prepare-blink.py`: checkout-derived About build information.
- `run-qemu.sh`: persistent, locked firmware profile.
- `tests/browser-qemu.py`, `tests/input-scenario.py`, `tests/input-pages/settings.html`, `tests/input-pages/settings-image.svg`: restart and visible behavior regressions.

This is a settings interface for the existing single-process firmware browser, not the complete Chromium settings implementation. Profile sync, extensions, password management, downloads UI, automatic updates and a process sandbox are not supplied. The interface is currently English. Settings changes rebuild the local settings document and reset its search filter. Physical firmware, other resolutions, save-failure rendering and accessibility assistive technologies were not tested in this run.
