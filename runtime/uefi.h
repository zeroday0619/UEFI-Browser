#ifndef UEFI_H
#define UEFI_H
#include <stddef.h>
#include <stdint.h>
typedef uint64_t Status;
typedef void *Handle;
typedef uint16_t Character;
#define UEFI_API __attribute__((ms_abi))
#define ERROR(n) (0x8000000000000000ULL | (n))
typedef struct {
  uint32_t first;
  uint16_t second, third;
  uint8_t last[8];
} Guid;
typedef struct {
  uint64_t signature;
  uint32_t revision, size, crc, reserved;
} TableHeader;
typedef struct {
  uint16_t scan;
  Character character;
} Key;
typedef struct Input Input;
struct Input {
  void *reset;
  Status(UEFI_API *read)(Input *, Key *);
  void *wait;
};
typedef struct Output Output;
struct Output {
  void *reset;
  Status(UEFI_API *write)(Output *, Character *);
  void *test;
  Status(UEFI_API *query)(Output *, size_t, size_t *, size_t *);
  void *set_mode, *set_attribute;
  Status(UEFI_API *clear)(Output *);
  void *set_cursor, *enable_cursor;
  void *mode;
};
/* Boot service indices preserve the standardized UEFI table layout. */
typedef struct {
  TableHeader header;
  void *function[44];
} BootServices;
typedef struct {
  TableHeader header;
  Character *vendor;
  uint32_t revision;
  Handle input_handle;
  Input *input;
  Handle output_handle;
  Output *output;
  Handle error_handle;
  Output *error;
  void *runtime;
  BootServices *boot;
  size_t table_count;
  void *tables;
} SystemTable;
typedef struct Binding Binding;
struct Binding {
  Status(UEFI_API *create)(Binding *, Handle *);
  Status(UEFI_API *destroy)(Binding *, Handle);
};
typedef struct {
  uint8_t automatic;
  uint8_t address[4], subnet[4];
  uint16_t port;
} AccessPoint;
typedef struct {
  uint32_t version, timeout;
  uint8_t ipv6;
  AccessPoint *access;
} HttpConfig;
typedef struct {
  uint32_t method;
  Character *url;
} Request;
typedef struct {
  uint32_t status;
} Response;
typedef struct {
  char *name, *value;
} Header;
typedef struct {
  void *data;
  size_t header_count;
  Header *headers;
  size_t length;
  void *body;
} Message;
typedef struct {
  void *event;
  Status status;
  Message *message;
} Token;
typedef struct Http Http;
struct Http {
  void *get_mode;
  Status(UEFI_API *configure)(Http *, HttpConfig *);
  Status(UEFI_API *request)(Http *, Token *);
  Status(UEFI_API *cancel)(Http *, Token *);
  Status(UEFI_API *response)(Http *, Token *);
  Status(UEFI_API *poll)(Http *);
};
_Static_assert(offsetof(SystemTable, boot) == 96, "System table ABI");
_Static_assert(offsetof(HttpConfig, access) == 16, "HTTP config ABI");
_Static_assert(sizeof(AccessPoint) == 12, "IPv4 access ABI");
#endif
