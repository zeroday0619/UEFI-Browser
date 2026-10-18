#ifndef UEFI_RESOURCE_H
#define UEFI_RESOURCE_H
#include "uefi.h"
#define RESOURCE_URL_LIMIT 4096
#define RESOURCE_BODY_LIMIT (8 * 1024 * 1024)
#ifdef __cplusplus
extern "C" {
#endif
/* Response storage belongs to the caller until resource_release is called. */
typedef struct {
  unsigned status_code;
  Header *headers;
  size_t header_count;
  unsigned char *body;
  size_t length;
  size_t encoded_body_length;
} ResourceResponse;
typedef struct ResourceJob ResourceJob;
int resource_async_available(void);
Status resource_async_initialize(SystemTable *table);
ResourceJob *resource_async_start(SystemTable *, const char *, const Header *, size_t);
void resource_async_process(void);
int resource_async_result(ResourceJob *, Status *, ResourceResponse *);
void resource_async_release(ResourceJob *);
void resource_async_shutdown(SystemTable *);
void resource_cooperate(void);
Status resource_get(SystemTable *table, const char *url,
                    ResourceResponse *response);
Status resource_get_with_headers(SystemTable *table, const char *url,
                                 const Header *headers, size_t header_count,
                                 ResourceResponse *response);
void resource_release(ResourceResponse *response);
int resource_is_redirect(unsigned status_code);
const char *resource_header(const ResourceResponse *response, const char *name);
int resource_decode_chunks(const char *wire, size_t size, char *output,
                           size_t capacity, size_t *length);
#ifdef __cplusplus
}
#endif
#endif
