# Large HTTP resources and partial TLS reads

A live YouTube investigation found a main JavaScript bundle of 10,877,807 bytes, larger than the firmware's former 8 MiB response limit. The response bound is now 32 MiB, independently of the unchanged 8 MiB upload bound. `resource.c` allocates the declared response size when known; otherwise it starts with 64 KiB and doubles up to the bound. Small responses no longer reserve the full maximum body buffer. Chunked responses are bounded by encoded wire bytes, so framing counts toward their maximum size.

This exposed two partial-read defects. The preliminary chunk scan must use the total response bound rather than the current allocation capacity because the next declared chunk may not have arrived yet. The final copying decode still uses the actual allocation capacity.

EDK II's HTTPS receive path parsed only the portion of a TLS fragment copied into the caller's buffer. It cached the remainder, but subsequent cache reads bypassed parsing. This caused parser state to skip bytes at buffer-growth boundaries and eventually abort valid responses. The `HttpImpl.c` section of `patches/edk2-http-cooperative.patch` now parses each full TLS fragment once, copies only the current message bytes that fit, and translates any next-message pointer into the cached remainder. The driver retains the original HTTP message-boundary parser.

## Verification

The EFI image in `build/large-resources-tls` has SHA-256 `cc3da64ec265b4ee658dcbbf9a4ab5b1efa256c109f3cbb41417bcbc3c15463f`; the payload hash is `aa0399d1ccdd46dedb6b0ef104ae53950774829ebf9ac576c28984ada7030a2b`. Native compilation, EDK II driver compilation, EFI linking and static validation (`large-resources-tls-binary.log`; local artifact) passed. The patch also passed reverse applicability checking against the pinned EDK II checkout.

The QEMU suite (`large-resources-tls-suite.json`; local artifact) passed at CPL3 without unexpected renderer faults:

- An 11 MiB script with declared Content-Length executes through Blink.
- An 11 MiB chunked HTTPS response preserves every byte across buffer growth and partial TLS reads.
- A declared response larger than 32 MiB is rejected, and a subsequent small request succeeds.
- Existing HTTPS method/CORS/redirect checks and MSE audio/video with captured AC’97 waveform validation still pass.

The earlier `large-resources-suite.json` and `large-resources-fixed-suite.json` retain the partial-read failures. The test server was corrected to emit HTTP/1.1 for chunked transfer coding before the second run; that change alone did not resolve the receive failure. The final run additionally includes the TLS parser fix. No request limit or cross-origin policy was relaxed.

```sh
python3 tests/network-servers.py --directory build/large-resources-tls-https
# Run the following command in another terminal while the server is running.
python3 tests/state-suite.py --cpus 4 --workers 2 \
  --build-directory build/large-resources-tls \
  --https-ca build/large-resources-tls-https/ca.esl \
  --prefix large-resources-tls --require-cpl3 \
  --require-no-renderer-faults large-resources network-secure mse-audio
```

Responses remain fully buffered before Blink delivery. Streaming delivery, per-origin scheduling, comprehensive malformed/pipelined TLS response coverage and aggregate network memory accounting remain incomplete. Increasing one response bound does not implement unlimited resource loading or establish full browser compatibility.

The verified image was promoted to `build/blink` with previous binaries retained under `build/release-backups`; see promotion hashes (`large-resources-default-promotion.json`; local artifact). A live YouTube rerun received the 10,877,847-byte main bundle successfully, but playback was still pending at 90 seconds. See [the live investigation](YOUTUBE.md).
