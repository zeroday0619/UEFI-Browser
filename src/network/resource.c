#include "resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static Guid standard_http_binding = {0xbdc8e6af,0xd9bc,0x4379,{0xa7,0x2a,0xe0,0xc4,0xe7,0x5d,0xae,0x1c}};
static Guid http_binding = {0xc1493c30,0x94c5,0x49b3,{0x94,0xe4,0xac,0xb5,0x02,0x39,0x11,0x81}};
static Guid http_protocol = {0x7a59b29b,
                             0x910b,
                             0x4171,
                             {0x82, 0x42, 0xa8, 0x5a, 0x0d, 0xf2, 0x5b, 0x5b}};
static Guid ip_config = {0x5b446ed1,
                         0xe30b,
                         0x4faa,
                         {0x87, 0x1a, 0x36, 0x54, 0xec, 0xa3, 0x60, 0x80}};
#define SERVICE(index, type) ((type)table->boot->function[index])
static uint64_t milliseconds(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (uint64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}
static void firmware_free(SystemTable *table, void *memory) {
  if (memory)
    SERVICE(6, Status(UEFI_API *)(void *))(memory);
}
static Status protocol(SystemTable *table, Handle handle, Guid *guid,
                       void **output) {
  return SERVICE(16, Status(UEFI_API *)(Handle, Guid *, void **))(handle, guid,
                                                             output);
}
static void UEFI_API completed(void *event, void *context) {
  (void)event;
  *(volatile int *)context = 1;
}
static Status transfer(SystemTable *table, Http *http, Message *message,
                       int receive) {
  volatile int done = 0;
  Token token = {0, ERROR(6), message};
  Status result =
      SERVICE(7, Status(UEFI_API *)(uint32_t, size_t, void(UEFI_API *)(void *, void *),
                               void *, void **))(0x200, 8, completed,
                                                 (void *)&done, &token.event);
  if (result)
    return result;
  result = receive ? http->response(http, &token) : http->request(http, &token);
  if (!result) {
    uint64_t deadline = milliseconds() + 15000;
    while (!done && milliseconds() < deadline) {
      resource_cooperate();
      http->poll(http);
      SERVICE(28, Status(UEFI_API *)(size_t))(1000);
    }
    if (!done) {
      /* Reset drains pending operations before the stack token is released. */
      Status cancelled = http->cancel(http, &token);
      Status reset = http->configure(http, NULL);
      if (!done && cancelled && reset) {
        fputs("FAIL: firmware retained an uncancellable HTTP token\n", stderr);
        abort();
      }
      result = ERROR(18);
    } else {
      result = token.status;
    }
  }
  SERVICE(11, Status(UEFI_API *)(void *))(token.event);
  return result;
}
static void release_headers(SystemTable *table, Message *message) {
  for (size_t index = 0; index < message->header_count; index++) {
    firmware_free(table, message->headers[index].name);
    firmware_free(table, message->headers[index].value);
  }
  firmware_free(table, message->headers);
  message->headers = NULL;
  message->header_count = 0;
}
int resource_is_redirect(unsigned status_code) {
  return status_code == 301 || status_code == 302 || status_code == 303 ||
         status_code == 307 || status_code == 308;
}
void resource_release(ResourceResponse *response) {
  for (size_t index = 0; index < response->header_count; index++) {
    free(response->headers[index].name);
    free(response->headers[index].value);
  }
  free(response->headers);
  free(response->body);
  memset(response, 0, sizeof(*response));
}
const char *resource_header(const ResourceResponse *response,
                            const char *name) {
  for (size_t index = 0; index < response->header_count; index++)
    if (!strcasecmp(response->headers[index].name, name))
      return response->headers[index].value;
  return NULL;
}
static Status copy_headers(ResourceResponse *response, const Message *message) {
  if (message->header_count > 256)
    return ERROR(4);
  response->headers = calloc(message->header_count, sizeof(Header));
  if (!response->headers && message->header_count)
    return ERROR(9);
  size_t total = 0;
  for (size_t index = 0; index < message->header_count; index++) {
    size_t name = strnlen(message->headers[index].name, 65537);
    size_t value = strnlen(message->headers[index].value, 65537);
    if (name + value > 65536 || total > 65536 - name - value)
      return ERROR(4);
    total += name + value;
    Header *header = &response->headers[index];
    response->header_count++;
    header->name = strdup(message->headers[index].name);
    header->value = strdup(message->headers[index].value);
    if (!header->name || !header->value)
      return ERROR(9);
  }
  return 0;
}
static Status prepare_network(SystemTable *table, Handle handle) {
  void **config = NULL;
  Status result = protocol(table, handle, &ip_config, (void **)&config);
  if (result)
    return result;
  uint32_t policy = 1;
  result = ((Status(UEFI_API *)(void *, uint32_t, size_t, void *))config[0])(
      config, 1, sizeof(policy), &policy);
  if (result && result != ERROR(6))
    return result;
  uint64_t deadline = milliseconds() + 15000;
  do {
    uint64_t information[512] = {0};
    size_t size = sizeof(information);
    result = ((Status(UEFI_API *)(void *, uint32_t, size_t *, void *))config[1])(
        config, 0, &size, information);
    /* EFI_IP4_CONFIG2_INTERFACE_INFO places StationAddress at offset 104. */
    unsigned char *address = (unsigned char *)information + 104;
    if (!result && (address[0] || address[1] || address[2] || address[3]))
      return 0;
    if (result && result != ERROR(6))
      return result;
    resource_cooperate();
    SERVICE(28, Status(UEFI_API *)(size_t))(1000);
  } while (milliseconds() < deadline);
  return ERROR(18);
}
static unsigned status_number(unsigned index) {
  static const unsigned codes[] = {
      0,   100, 101, 200, 201, 202, 203, 204, 205, 206, 300, 301, 302, 303, 304,
      305, 307, 400, 401, 402, 403, 404, 405, 406, 407, 408, 409, 410, 411, 412,
      413, 414, 415, 416, 417, 500, 501, 502, 503, 504, 505, 308, 429};
  return index < sizeof(codes) / sizeof(codes[0]) ? codes[index] : 0;
}
Status resource_get(SystemTable *table, const char *url,
                    ResourceResponse *response) {
  return resource_get_with_headers(table, url, NULL, 0, response);
}

Status resource_get_with_headers(SystemTable *table, const char *url,
                                 const Header *extra_headers, size_t extra_count,
                                 ResourceResponse *response) {
  memset(response, 0, sizeof(*response));
  if (extra_count > 64 || (extra_count && !extra_headers))
    return ERROR(2);
  size_t header_bytes = 0;
  for (size_t index = 0; index < extra_count; ++index) {
    const Header *header = &extra_headers[index];
    if (!header->name || !header->value || !*header->name)
      return ERROR(2);
    size_t name_size = strnlen(header->name, 256);
    size_t value_size = strnlen(header->value, 8192);
    if (name_size == 256 || value_size == 8192)
      return ERROR(2);
    header_bytes += name_size + value_size;
    if (header_bytes > 32768)
      return ERROR(2);
    for (size_t character = 0; character < name_size; ++character) {
      unsigned char value = header->name[character];
      if (!((value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') ||
            strchr("!#$%&'*+-.^_`|~", value)))
        return ERROR(2);
    }
    for (size_t character = 0; character < value_size; ++character) {
      unsigned char value = header->value[character];
      if ((value < 32 && value != '\t') || value == 127)
        return ERROR(2);
    }
    const char *reserved[] = {"host", "connection", "content-length",
                              "transfer-encoding", "accept-encoding", "upgrade"};
    for (size_t reserved_index = 0; reserved_index < 6; ++reserved_index)
      if (!strcasecmp(header->name, reserved[reserved_index]))
        return ERROR(2);
    for (size_t previous = 0; previous < index; ++previous)
      if (!strcasecmp(header->name, extra_headers[previous].name))
        return ERROR(2);
  }
  size_t size = strnlen(url, RESOURCE_URL_LIMIT);
  size_t start = !strncmp(url, "https://", 8)  ? 8
                 : !strncmp(url, "http://", 7) ? 7
                                               : 0;
  if (!start || size >= RESOURCE_URL_LIMIT || size <= start)
    return ERROR(2);
  Character wide_url[RESOURCE_URL_LIMIT];
  char host[RESOURCE_URL_LIMIT];
  size_t end = start;
  while (end < size && url[end] != '/' && url[end] != '?' && url[end] != '#')
    end++;
  if (end == start)
    return ERROR(2);
  for (size_t index = 0; index < size; index++) {
    if ((unsigned char)url[index] <= 32 || (unsigned char)url[index] >= 127 ||
        url[index] == '#' || url[index] == '\\' ||
        (index < end && url[index] == '@'))
      return ERROR(2);
    wide_url[index] = (unsigned char)url[index];
  }
  if (end == size || url[end] == '?') {
    if (size + 1 >= RESOURCE_URL_LIMIT)
      return ERROR(2);
    memmove(wide_url + end + 1, wide_url + end,
            (size - end) * sizeof(Character));
    wide_url[end] = '/';
    size++;
  }
  wide_url[size] = 0;
  memcpy(host, url + start, end - start);
  host[end - start] = 0;
  Guid *binding_identifier = resource_async_available() ? &http_binding : &standard_http_binding;
  Handle *handles = NULL, child = NULL;
  size_t count = 0;
  Binding *binding = NULL;
  Http *http = NULL;
  Status result =
      SERVICE(36, Status(UEFI_API *)(int, Guid *, void *, size_t *, Handle **))(
          2, binding_identifier, NULL, &count, &handles);
  if (!result && !count)
    result = ERROR(14);
  if (!result)
    result = protocol(table, handles[0], binding_identifier, (void **)&binding);
  if (!result)
    result = prepare_network(table, handles[0]);
  firmware_free(table, handles);
  if (result)
    return result;
  result = binding->create(binding, &child);
  if (result)
    return result;
  result = protocol(table, child, &http_protocol, (void **)&http);
  if (result)
    goto finish;
  AccessPoint access = {1, {0}, {0}, 0};
  HttpConfig config = {1, 10000, 0, &access};
  result = http->configure(http, &config);
  if (result)
    goto finish;
  Header headers[69] = {{"Host", host},
                      {"User-Agent", "UEFI-Browser/0.3"},
                      {"Accept", "*/*"},
                      {"Accept-Encoding", "identity"},
                      {"Connection", "close"}};
  size_t request_header_count = 5;
  for (size_t index = 0; index < extra_count; ++index) {
    if (!strcasecmp(extra_headers[index].name, "accept"))
      headers[2] = extra_headers[index];
    else
      headers[request_header_count++] = extra_headers[index];
  }
  Request request = {0, wide_url};
  Message message = {&request, request_header_count, headers, 0, NULL};
  result = transfer(table, http, &message, 0);
  if (result)
    goto finish;
  Response firmware_response = {0};
  message = (Message){&firmware_response, 0, NULL, 0, NULL};
  result = transfer(table, http, &message, 1);
  if (!result || result == ERROR(35)) {
    response->status_code = status_number(firmware_response.status);
    result =
        response->status_code ? copy_headers(response, &message) : ERROR(3);
  }
  release_headers(table, &message);
  if (result)
    goto finish;
  size_t expected = 0;
  int known = 0, chunked = 0;
  for (size_t index = 0; index < response->header_count; index++) {
    const Header *header = &response->headers[index];
    if (!strcasecmp(header->name, "content-length")) {
      if (known || !header->value[0]) {
        result = ERROR(24);
        goto finish;
      }
      known = 1;
      for (const char *digit = header->value; *digit; digit++) {
        if (*digit < '0' || *digit > '9' ||
            expected > RESOURCE_BODY_LIMIT / 10) {
          result = ERROR(24);
          goto finish;
        }
        expected = expected * 10 + (size_t)(*digit - '0');
        if (expected > RESOURCE_BODY_LIMIT) {
          result = ERROR(4);
          goto finish;
        }
      }
    } else if (!strcasecmp(header->name, "transfer-encoding")) {
      if (chunked || strcasecmp(header->value, "chunked")) {
        result = ERROR(3);
        goto finish;
      }
      chunked = 1;
    } else if (!strcasecmp(header->name, "content-encoding") &&
               strcasecmp(header->value, "identity")) {
      result = ERROR(3);
      goto finish;
    }
  }
  if (known && chunked) {
    result = ERROR(24);
    goto finish;
  }
  /* Redirects remain visible so the navigation layer can apply origin policy.
   */
  if (response->status_code == 204 || response->status_code == 205 ||
      response->status_code == 304 ||
      resource_is_redirect(response->status_code))
    goto finish;
  response->body = malloc(RESOURCE_BODY_LIMIT + 1);
  if (!response->body) {
    result = ERROR(9);
    goto finish;
  }
  uint64_t deadline = milliseconds() + 60000;
  int complete = known && !expected;
  while (!complete && response->length < RESOURCE_BODY_LIMIT &&
         milliseconds() < deadline) {
    size_t available = RESOURCE_BODY_LIMIT - response->length;
    if (known && available > expected - response->length)
      available = expected - response->length;
    message =
        (Message){NULL, 0, NULL, available, response->body + response->length};
    result = transfer(table, http, &message, 1);
    /* EDK II repeats the HTTP status error while delivering an error body. */
    if (result == ERROR(35) && response->status_code >= 400)
      result = 0;
    release_headers(table, &message);
    if ((result == ERROR(31) || result == ERROR(104)) && !known && !chunked) {
      result = 0;
      complete = 1;
      break;
    }
    if (result)
      break;
    if (message.length > available) {
      result = ERROR(24);
      break;
    }
    response->length += message.length;
    response->encoded_body_length += message.length;
    if (chunked) {
      size_t decoded = 0;
      int state =
          resource_decode_chunks((char *)response->body, response->length, NULL,
                                 RESOURCE_BODY_LIMIT + 1, &decoded);
      if (state < 0) {
        result = ERROR(24);
        break;
      }
      if (state) {
        resource_decode_chunks((char *)response->body, response->length,
                               (char *)response->body, RESOURCE_BODY_LIMIT + 1,
                               &decoded);
        response->length = decoded;
        complete = 1;
      }
    } else if (known) {
      complete = response->length == expected;
    } else if (!message.length) {
      complete = 1;
    }
    if (!message.length && !complete) {
      result = ERROR(24);
      break;
    }
  }
  if (!result && !complete)
    result = ERROR(4);
  response->body[response->length] = 0;
finish:
  if (http)
    http->configure(http, NULL);
  Status destroyed = binding->destroy(binding, child);
  if (!result)
    result = destroyed;
  if (result)
    resource_release(response);
  return result;
}
