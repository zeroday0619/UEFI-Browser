#include "resource.h"
int resource_decode_chunks(const char *wire, size_t size, char *output,
                           size_t capacity, size_t *length) {
  size_t cursor = 0, total = 0;
  for (;;) {
    size_t end = cursor;
    while (end + 1 < size && !(wire[end] == '\r' && wire[end + 1] == '\n'))
      end++;
    if (end + 1 >= size)
      return 0;
    size_t amount = 0, digits = 0;
    for (size_t index = cursor; index < end && wire[index] != ';'; index++) {
      unsigned char value = (unsigned char)wire[index];
      unsigned int digit;
      if (value >= '0' && value <= '9')
        digit = value - '0';
      else if (value >= 'a' && value <= 'f')
        digit = value - 'a' + 10;
      else if (value >= 'A' && value <= 'F')
        digit = value - 'A' + 10;
      else
        return -1;
      if (amount > ((size_t)-1 - digit) / 16)
        return -1;
      amount = amount * 16 + digit;
      digits++;
    }
    if (!digits || !capacity || amount >= capacity ||
        total >= capacity - amount)
      return -1;
    cursor = end + 2;
    if (!amount) {
      for (;;) {
        end = cursor;
        while (end + 1 < size && !(wire[end] == '\r' && wire[end + 1] == '\n'))
          end++;
        if (end + 1 >= size)
          return 0;
        if (end == cursor) {
          if (end + 2 != size)
            return -1;
          if (output)
            output[total] = 0;
          *length = total;
          return 1;
        }
        cursor = end + 2;
      }
    }
    if (amount > size - cursor || size - cursor - amount < 2)
      return 0;
    if (wire[cursor + amount] != '\r' || wire[cursor + amount + 1] != '\n')
      return -1;
    if (output)
      for (size_t index = 0; index < amount; index++)
        output[total + index] = wire[cursor + index];
    total += amount;
    cursor += amount + 2;
  }
}
