#include "../src/network/resource.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  const char wire[] = "3\r\na\0b\r\n2\r\ncd\r\n0\r\n\r\n";
  const char expected[] = "a\0bcd";
  size_t length = 0;
  for (size_t prefix = 0; prefix < sizeof(wire) - 1; prefix++)
    assert(resource_decode_chunks(wire, prefix, NULL, 64, &length) == 0);
  char output[64];
  assert(resource_decode_chunks(wire, sizeof(wire) - 1, output, sizeof(output),
                                &length) == 1);
  assert(length == 5 && !memcmp(output, expected, 5));
  memcpy(output, wire, sizeof(wire));
  assert(resource_decode_chunks(output, sizeof(wire) - 1, output,
                                sizeof(output), &length) == 1);
  assert(length == 5 && !memcmp(output, expected, 5));
  assert(resource_decode_chunks(wire, sizeof(wire) - 1, NULL, 5, &length) ==
         -1);
  const char *invalid[] = {"q\r\nx\r\n0\r\n\r\n", "1\r\nxXX0\r\n\r\n",
                           "0\r\n\r\njunk", "fffffffffffffffff\r\n"};
  for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++)
    assert(resource_decode_chunks(invalid[index], strlen(invalid[index]), NULL,
                                  64, &length) == -1);
  puts("PASS: all truncated prefixes, binary data, in-place decoding, capacity "
       "and malformed chunks");
}
