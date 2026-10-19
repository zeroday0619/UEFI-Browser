#!/usr/bin/env python3
"""Exercise the actual position-independent worker pixel loop on the host CPU."""
from pathlib import Path
import subprocess
import tempfile
project = Path(__file__).resolve().parents[1]
source = (project / 'src/process/entry.S').read_text().split('process_program_start:\n', 1)[1].split('process_program_end:', 1)[0]
# Only the privilege-transition terminator changes; the pixel instructions are unchanged.
source = '.text\n.global process_pixels\nprocess_pixels:\n push %rbx\n' + source.replace(' int $0x80\n', ' pop %rbx\n ret\n') + '\n.section .note.GNU-stack,"",@progbits\n'
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    (directory / 'pixels.S').write_text(source)
    (directory / 'test.c').write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern void process_pixels(const uint64_t*);
int main(void) {
  _Alignas(16) uint32_t source[2048], output[4096];
  for (unsigned index = 0; index < 2048; ++index) source[index] = 0xa5abcdefu ^ (index * 1234567);
  for (unsigned width = 1; width <= 129; ++width)
  for (unsigned offset = 0; offset < 4; ++offset)
  for (unsigned padding = 0; padding < 4; ++padding)
  for (unsigned red = 0; red < 2; ++red) {
    unsigned stride = width + padding, rows = 9;
    memset(output, 0x5a, sizeof(output));
    uint64_t command[] = {0, (uintptr_t)source, (uintptr_t)(output + offset), width, rows, stride, red, 0};
    process_pixels(command);
    for (unsigned index = 0; index < 4096; ++index) {
      uint32_t expected = 0x5a5a5a5a;
      if (index >= offset && (index-offset)/stride < rows && (index-offset)%stride < width) {
        expected = source[(index-offset)/stride*width + (index-offset)%stride];
        if (red) expected = ((expected & 255) << 16) | (expected & 0xff00) | ((expected >> 16) & 255);
      }
      assert(output[index] == expected);
    }
  }
  puts("PASS: actual SSE2 worker instructions, 4128 width/alignment/stride/format cases and output guards");
}
''')
    subprocess.run(['clang', '-O2', '-Wall', '-Wextra', '-Werror', str(directory/'test.c'), str(directory/'pixels.S'), '-o', str(directory/'test')], check=True)
    subprocess.run([str(directory/'test')], check=True)
