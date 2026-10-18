#include "platform/runtime.h"
#include "resource.h"
#include <stdio.h>
#include <string.h>

int run_network_probe(SystemTable *table) {
  static const struct {
    const char *path;
    unsigned status;
    const char *body;
    size_t length;
  } cases[] = {
      {"document", 200,
       "<!doctype html><title>UEFI network</title><p>Live HTTP</p>", 58},
      {"chunked", 200, "chunk-one\0chunk-two", 19},
      {"missing", 404, "missing", 7},
      {"redirect", 302, "", 0},
      {"empty", 204, "", 0},
      {"choices", 300, "choices", 7},
  };
  for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
    char url[128];
    snprintf(url, sizeof(url), "http://10.0.2.2:18765/%s", cases[index].path);
    size_t allocated_before = runtime_memory_used();
    ResourceResponse response;
    Status result = resource_get(table, url, &response);
    printf("NETWORK: %s transport=0x%llx status=%u bytes=%zu\n",
           cases[index].path, (unsigned long long)result, response.status_code,
           response.length);
    int valid = !result && response.status_code == cases[index].status &&
                response.length == cases[index].length &&
                (!response.length ||
                 !memcmp(response.body, cases[index].body, response.length));
    if (index == 0) {
      const char *type = resource_header(&response, "Content-Type");
      valid = valid && type && !strcmp(type, "text/html; charset=utf-8");
    }
    if (index == 3) {
      const char *location = resource_header(&response, "Location");
      valid = valid && location && !strcmp(location, "/document");
    }
    resource_release(&response);
    valid = valid && allocated_before == runtime_memory_used();
    if (!valid) {
      puts("FAIL: native HTTP resource validation");
      return 1;
    }
  }
  puts("PASS: native HTTP document, binary chunks, 404, redirect and empty "
       "response");
  static const char *failures[] = {"truncated", "bad-chunk", "oversized",
                                   "encoded"};
  for (size_t index = 0; index < sizeof(failures) / sizeof(failures[0]);
       index++) {
    char url[128];
    snprintf(url, sizeof(url), "http://10.0.2.2:18765/%s", failures[index]);
    size_t allocated_before = runtime_memory_used();
    ResourceResponse response;
    Status result = resource_get(table, url, &response);
    printf("NETWORK: %s rejected=0x%llx\n", failures[index],
           (unsigned long long)result);
    int valid =
        result && !response.body && !response.headers && !response.length;
    resource_release(&response);
    if (!valid || allocated_before != runtime_memory_used()) {
      puts("FAIL: resource failure cleanup");
      return 1;
    }
  }
  puts("PASS: truncated, malformed, oversized and encoded resources rejected "
       "without leaks");
  Header request_headers[] = {{"Accept", "text/css"},
                              {"X-UEFI-Probe", "header-forwarding"}};
  ResourceResponse forwarded;
  Status forwarded_status = resource_get_with_headers(
      table, "http://10.0.2.2:18765/headers", request_headers, 2, &forwarded);
  int headers_valid = !forwarded_status && forwarded.status_code == 200 &&
                      forwarded.length == 7 &&
                      !memcmp(forwarded.body, "headers", 7);
  resource_release(&forwarded);
  if (!headers_valid) {
    puts("FAIL: request headers were not forwarded correctly");
    return 1;
  }
  Header invalid_headers[] = {{"X-Bad\r\nInjected", "value"},
                              {"X-Probe", "value\r\nInjected: yes"},
                              {"Host", "different.example"},
                              {"Content-Length", "1"},
                              {"Transfer-Encoding", "chunked"},
                              {"Accept-Encoding", "gzip"},
                              {"Bad Name", "value"}};
  for (size_t index = 0; index < sizeof(invalid_headers) / sizeof(invalid_headers[0]);
       ++index) {
    ResourceResponse rejected;
    Status rejected_status = resource_get_with_headers(
        table, "http://10.0.2.2:18765/must-not-send", &invalid_headers[index], 1,
        &rejected);
    int valid = rejected_status && !rejected.body && !rejected.headers;
    resource_release(&rejected);
    if (!valid) {
      puts("FAIL: unsafe request header accepted");
      return 1;
    }
  }
  puts("PASS: request headers forwarded; unsafe headers rejected before transport");
  ResourceResponse untrusted;
  Status untrusted_status =
      resource_get(table, "https://10.0.2.2:18766/document", &untrusted);
  resource_release(&untrusted);
  if (!untrusted_status) {
    puts("FAIL: untrusted TLS certificate accepted");
    return 1;
  }
  printf("PASS: untrusted TLS endpoint rejected (0x%llx)\n",
         (unsigned long long)untrusted_status);
#ifdef NETWORK_URL
  ResourceResponse response;
  Status result = resource_get(table, NETWORK_URL, &response);
  printf("NETWORK: %s transport=0x%llx status=%u bytes=%zu\n", NETWORK_URL,
         (unsigned long long)result, response.status_code, response.length);
  const char *type = resource_header(&response, "content-type");
  int valid = !result && response.status_code == 200 && response.length &&
              type && !strncmp(type, "text/html", 9);
  resource_release(&response);
  if (!valid) {
    puts("FAIL: live document download");
    return 1;
  }
  puts(
      "PASS: live document download; Blink rendering remains unverified");
#endif
  return 0;
}
