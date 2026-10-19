#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <sys/socket.h>
static struct sockaddr_in dns_destinations[4096];
/* Redirect only this test process's DNS datagrams to the loopback fixture. */
ssize_t sendto(int descriptor, const void *buffer, size_t length, int flags,
               const struct sockaddr *address, socklen_t size) {
  static ssize_t (*original)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
  if (!original) original = dlsym(RTLD_NEXT, "sendto");
  if (address && address->sa_family == AF_INET && size >= sizeof(struct sockaddr_in)) {
    struct sockaddr_in destination = *(const struct sockaddr_in *)address;
    if (destination.sin_port == htons(53) && descriptor >= 0 && descriptor < 4096) {
      dns_destinations[descriptor] = destination;
      destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      destination.sin_port = htons(15353);
      return original(descriptor, buffer, length, flags, (const struct sockaddr *)&destination, sizeof(destination));
    }
  }
  return original(descriptor, buffer, length, flags, address, size);
}

ssize_t recvfrom(int descriptor, void *buffer, size_t length, int flags,
                 struct sockaddr *address, socklen_t *size) {
  static ssize_t (*original)(int, void *, size_t, int, struct sockaddr *, socklen_t *);
  if (!original) original = dlsym(RTLD_NEXT, "recvfrom");
  ssize_t result = original(descriptor, buffer, length, flags, address, size);
  if (result >= 0 && address && size && *size >= sizeof(struct sockaddr_in) &&
      address->sa_family == AF_INET && descriptor >= 0 && descriptor < 4096) {
    struct sockaddr_in *source = (struct sockaddr_in *)address;
    if (source->sin_addr.s_addr == htonl(INADDR_LOOPBACK) && source->sin_port == htons(15353) &&
        dns_destinations[descriptor].sin_port)
      *source = dns_destinations[descriptor];
  }
  return result;
}
