#include "firmware_io.h"
#include "renderer_runtime.h"
#include "uefi.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* These structures match EFI_DNS4_PROTOCOL in the pinned EDK II tree. */
typedef struct {
  uint8_t bytes[4];
} Address;
typedef struct {
  size_t server_count;
  Address *servers;
  uint8_t automatic, cache, protocol;
  Address station, subnet;
  uint16_t port;
  uint32_t retries, interval;
} DnsConfig;
typedef struct {
  uint32_t count;
  Address *addresses;
} DnsResponse;
typedef struct {
  void *event;
  Status status;
  uint32_t retries, interval;
  DnsResponse *response;
} DnsToken;
typedef struct Dns Dns;
struct Dns {
  void *mode;
  Status(UEFI_API *configure)(Dns *, DnsConfig *);
  Status(UEFI_API *resolve)(Dns *, Character *, DnsToken *);
  void *reverse, *general, *update;
  Status(UEFI_API *poll)(Dns *);
  Status(UEFI_API *cancel)(Dns *, DnsToken *);
};
_Static_assert(sizeof(DnsConfig) == 40 && offsetof(DnsConfig, retries) == 32,
               "DNS4 configuration ABI");
_Static_assert(sizeof(DnsToken) == 32 && offsetof(DnsToken, response) == 24,
               "DNS4 token ABI");
#define DNS_COUNT 16
#define CALL(index, type) ((type)table->boot->function[index])
extern SystemTable *platform_firmware(void);
static Guid binding_identifier = {
    0xb625b186,
    0xe063,
    0x44f7,
    {0x89, 0x05, 0x6a, 0x74, 0xdc, 0x6f, 0x52, 0xb4}};
static Guid protocol_identifier = {
    0xae3d28cc,
    0xe05b,
    0x4fa1,
    {0xa0, 0x11, 0x7e, 0xb5, 0x5a, 0x3f, 0x14, 0x01}};
typedef struct {
  unsigned owner;
  Binding *binding;
  Handle child;
  Dns *dns;
  DnsToken token;
  Character hostname[254];
  volatile int done;
  int started;
  time_t deadline;
} Query;
static Query *queries[DNS_COUNT];
static void UEFI_API complete(void *event, void *context) {
  (void)event;
  *(volatile int *)context = 1;
}
static long release(SystemTable *table, unsigned index) {
  Query *query = queries[index];
  if (query->dns) {
    query->dns->cancel(query->dns, NULL);
    Status status = query->dns->configure(query->dns, NULL);
    if (status || (query->started && !query->done))
      return -EBUSY;
  }
  if (query->child && query->binding->destroy(query->binding, query->child))
    return -EBUSY;
  if (query->token.event)
    CALL(11, Status(UEFI_API *)(void *))(query->token.event);
  if (query->token.response) {
    if (query->token.response->addresses)
      CALL(6, Status(UEFI_API *)(void *))(query->token.response->addresses);
    CALL(6, Status(UEFI_API *)(void *))(query->token.response);
  }
  queries[index] = NULL;
  free(query);
  return 0;
}
void firmware_dns_reset(unsigned owner) {
  SystemTable *table = platform_firmware();
  for (unsigned index = 0; index < DNS_COUNT; ++index)
    if (queries[index] && queries[index]->owner == owner)
      release(table, index);
}
long firmware_dns_service(unsigned owner, unsigned operation, long handle,
                          void *buffer) {
  SystemTable *table = platform_firmware();
  if (operation == 0) {
    const char *hostname = buffer;
    size_t length = hostname ? strnlen(hostname, 254) : 0;
    if (!length || length > 253)
      return -EINVAL;
    for (size_t index = 0; index < length; ++index) {
      unsigned char value = hostname[index];
      if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '.'))
        return -EINVAL;
    }
    unsigned index;
    for (index = 0; index < DNS_COUNT && queries[index]; ++index) {
    }
    if (index == DNS_COUNT)
      return -EMFILE;
    Query *query = calloc(1, sizeof(*query));
    if (!query)
      return -ENOMEM;
    query->owner = owner;
    query->deadline = time(NULL) + 20;
    queries[index] = query;
    for (size_t position = 0; position < length; ++position)
      query->hostname[position] = (unsigned char)hostname[position];
    Handle *handles = NULL;
    size_t count = 0;
    Status status =
        CALL(36, Status(UEFI_API *)(int, Guid *, void *, size_t *, Handle **))(
            2, &binding_identifier, NULL, &count, &handles);
    if (!status && !count)
      status = ERROR(14);
    if (!status)
      status = CALL(16, Status(UEFI_API *)(Handle, Guid *, void **))(
          handles[0], &binding_identifier, (void **)&query->binding);
    if (handles)
      CALL(6, Status(UEFI_API *)(void *))(handles);
    if (!status)
      status = query->binding->create(query->binding, &query->child);
    if (!status)
      status = CALL(16, Status(UEFI_API *)(Handle, Guid *, void **))(
          query->child, &protocol_identifier, (void **)&query->dns);
    if (!status)
      status = CALL(7, Status(UEFI_API *)(uint32_t, size_t,
                                          void(UEFI_API *)(void *, void *),
                                          void *, void **))(
          0x200, 8, complete, (void *)&query->done, &query->token.event);
    if (status) {
      release(table, index);
      return -EIO;
    }
    return index + 1;
  }
  if (handle < 1 || handle > DNS_COUNT || !queries[handle - 1] ||
      queries[handle - 1]->owner != owner)
    return -EBADF;
  Query *query = queries[handle - 1];
  if (operation == 2)
    return release(table, handle - 1);
  if (operation != 1 || !buffer)
    return -EINVAL;
  if (time(NULL) > query->deadline)
    return -ETIMEDOUT;
  if (!query->started) {
    DnsConfig config = {.automatic = 1,
                        .cache = 1,
                        .protocol = 17,
                        .retries = 2,
                        .interval = 2};
    Status status = query->dns->configure(query->dns, &config);
    if (status == ERROR(17))
      return 0;
    if (status)
      return -EIO;
    query->token.status = ERROR(6);
    status = query->dns->resolve(query->dns, query->hostname, &query->token);
    if (status)
      return -EIO;
    query->started = 1;
  }
  query->dns->poll(query->dns);
  if (!query->done)
    return 0;
  if (query->token.status || !query->token.response ||
      !query->token.response->count || !query->token.response->addresses)
    return -ENOENT;
  memcpy(buffer, query->token.response->addresses[0].bytes, 4);
  return 1;
}
long firmware_dns_request(unsigned operation, long handle, void *buffer) {
  if (renderer_runtime_is_child())
    return (long)renderer_runtime_request(RENDERER_DNS_REQUEST, operation,
                                          handle, (uintptr_t)buffer, 0, 0);
  return firmware_dns_service(0, operation, handle, buffer);
}
