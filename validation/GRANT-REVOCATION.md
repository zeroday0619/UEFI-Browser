# Grant revocation cost

## Reproduction and change

The live YouTube Inspector repeatedly displayed its tab strip without accepting Console input within the observer deadline. The `inspector-user-stack` failure snapshot caught the BSP at EFI address `0x14000636d`, inside the leaf loop of `renderer_revoke_aliases`, while the AP waited for broker operation 2 (`FREE_POOL`). Disassembly against the matching EFI image identifies the recursive walk. This is a sampled execution path, not proof of a deadlock or the only latency source.

Previously every physical grant release walked the entire renderer arena, including ordinary heap allocations that had never been mapped there. `RendererGrant` now contains a supervisor-owned `may_have_aliases` marker. The mapping path sets it before publishing an arena mapping. It remains set after unmap, remap or mapping failure. Grant release skips the arena walk only if the marker has never been set; marked grants retain the original full revocation path. Moving a grant record preserves the marker, and new grants initialize it to zero. No renderer-provided alias count controls this decision.

The change does not alter executable permissions, JIT scope handling, the physical memory quota or the lifetime rules of shared buffers. It does not establish a complete sandbox audit.

## Validation

The normal EFI image is `2d882dbfd9612044c6d543175c460c7cb38190b6954d01fea8e4a766bd3ed2ae`. Its payload is unchanged from the memory-pressure image: `a8c0f734b41a4b680bccd523266aa90708ddee5d1088e56834a4fd18d998226d`.

`grant-revoke-probe-suite.json` passes all 14 CPL3 boundary cases, live supervisor-access fault recovery, repeated physical reclamation and sparse page-table growth. The extended memory probe allocates broker backing, installs present and non-present aliases, frees that backing and checks that the aliases expose no readable user mapping and the identity mapping loses user access.

`grant-revoke-normal-suite.json` passes DevTools interaction, asynchronous scrolling, workers, WebGL and WebGPU at CPL3 without unexpected renderer termination. EFI linking and `validate-binary.py` pass. These checks use isolated test images; the default `build/blink` image has not been replaced.

The controlled allocation benchmark uses the same probe payload with either the previous loader object or the modified loader. After creating 2,304 sparse mappings, it allocates, writes, checks and frees an 8 KiB ordinary heap block 1,024 times. The benchmark measures this path only, not general browser or playback speed.

The sequential baseline and modified runs both completed successfully with identical payload SHA-256 `b1e0204968e42bba9573be3b2dd4d6606f8799a81a90b2441d90019ecc33cd5d`. No other QEMU instance ran during the comparison. `grant-revoke-benchmark-results.json` retains all measurements in microseconds:

| Sample order within image | Previous loader | Modified loader |
| --- | ---: | ---: |
| 1 | 245,135 | 6,010 |
| 2 | 245,956 | 6,082 |
| 3 | 244,203 | 6,024 |
| 4 | 247,097 | 6,059 |
| 5 | 501,283 | 12,202 |

These are repeated probe invocations within one boot per image, not five independent browser benchmarks. The result supports reduced ordinary-grant release cost in the populated arena. Live-site interaction is checked separately.

## Live-site observer correction

The first modified-image run reached 180 seconds without renderer termination but failed the Console-prompt assertion. Its captured Elements underline moved from rows 104–106 before the click to rows 144–146 afterward, as the language banner appeared. The cursor remained at the old row. This run therefore does not establish an Inspector input-dispatch failure.

The observer now recalculates the click row only if a later screenshot shows that the Elements underline moved. It retains the original 30-second Console deadline. It does not retry unchanged coordinates or treat a loaded frontend as a working Console. The earlier `pointer-trace-second` screenshot has no banner and shows the pointer on the actual Console tab, so this newly verified coordinate race does not by itself explain every historical failure.

The corrected-coordinate run in `build/grant-revoke-console` also reached 180 seconds without renderer termination, but the Console prompt still did not appear within 30 seconds. Its screenshot places the cursor over the Console tab at the updated position. The failure stack catches CPL3 `virtual_memory_map` scanning reservation intervals while V8 allocates a new semispace page. The current unsorted interval algorithm rescans all 4,096 slots each time its candidate overlaps another region; reducing that reservation-search cost remains separate work. These observations do not prove working live-site Console input or uninterrupted YouTube playback.

## Final publication order

The final loader marks the grant before a possible BSP page-table-reserve wait, so it does not retain an unmarked grant across that wait. `build/grant-revoke-final` has EFI SHA-256 `8dde8aed60ac01572ef030973bdb551f8adb73638e63b4895037c3bf443e5f7a`; its payload is unchanged. The final probe includes the timing loop. `grant-revoke-final-probe-suite.json` verifies the 14 boundary cases, alias revocation, sparse growth and fault recovery on this ordering. `grant-revoke-final-normal-suite.json` records the final-image DevTools and WebGPU checks. Static binary validation passes in `grant-revoke-final-binary.log`.

The live-site runs and controlled timing comparison above use the first experimental loader ordering. They are not relabeled as tests of the final EFI hash. `grant-revoke-sources.json` distinguishes these images and their sources. The default image remains unchanged.

## Reproduction commands

From the project directory:

```sh
ENABLE_BLINK=1 BUILD_DIRECTORY="$PWD/build/grant-revoke-final" ./build-musl.sh
ENABLE_BLINK=1 ENABLE_MEMORY_PROBE=1 ENABLE_CPL3_PROBE=1 \
  BUILD_DIRECTORY="$PWD/build/grant-revoke-final-probe" ./build-musl.sh
BUILD_DIRECTORY="$PWD/build/grant-revoke-final" python3 validate-binary.py
```

Run `python3 tests/network-servers.py --directory build/grant-revoke-final-https` in a separate terminal, then use fresh prefixes for another test run:

```sh
python3 tests/state-suite.py --workers 1 --cpus 4 \
  --https-ca build/grant-revoke-final-https/ca.esl \
  --build-directory build/grant-revoke-final-probe --prefix grant-revoke-probe-repeat \
  --require-cpl3-probe --require-memory-probe sandbox
python3 tests/state-suite.py --workers 1 --cpus 4 \
  --https-ca build/grant-revoke-final-https/ca.esl \
  --build-directory build/grant-revoke-final --prefix grant-revoke-normal-repeat \
  --require-cpl3 --require-no-renderer-faults devtools webgpu
```
