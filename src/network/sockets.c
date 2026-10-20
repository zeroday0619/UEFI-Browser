#define _GNU_SOURCE
#include "uefi.h"
#include "renderer_runtime.h"
#include "firmware_io.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* These layouts match EFI_TCP4_PROTOCOL from the pinned EDK II headers. */
typedef struct { uint8_t automatic, address[4], subnet[4]; uint16_t port;
  uint8_t remote[4]; uint16_t remote_port; uint8_t active; } TcpAccess;
typedef struct { uint32_t receive, send, backlog, timeout, retries, fin, time_wait,
  keep_probes, keep_time, keep_interval; uint8_t nagle, timestamp, scale, sack, path_mtu; } TcpOptions;
typedef struct { uint8_t service, ttl; TcpAccess access; TcpOptions *options; } TcpConfig;
typedef struct { void *event; Status status; } Completion;
typedef struct { uint32_t length; void *buffer; } Fragment;
typedef struct { uint8_t first, second; uint32_t length, count; Fragment fragment; } Packet;
typedef struct { Completion completion; Packet *packet; } IoToken;
typedef struct Tcp Tcp;
struct Tcp {
  Status(UEFI_API *mode)(Tcp *, int *, TcpConfig *, void *, void *, void *);
  Status(UEFI_API *configure)(Tcp *, TcpConfig *);
  void *routes;
  Status(UEFI_API *connect)(Tcp *, Completion *);
  void *accept;
  Status(UEFI_API *transmit)(Tcp *, IoToken *);
  Status(UEFI_API *receive)(Tcp *, IoToken *);
  void *close;
  Status(UEFI_API *cancel)(Tcp *, Completion *);
  Status(UEFI_API *poll)(Tcp *);
};
_Static_assert(sizeof(TcpAccess) == 20 && offsetof(TcpConfig, options) == 24, "TCP4 configuration ABI");
_Static_assert(offsetof(Packet, fragment) == 16 && sizeof(IoToken) == 24, "TCP4 packet ABI");
#define SOCKET_BASE 1024
#define SOCKET_COUNT 32
#define SOCKET_BUFFER 65536
#define CALL(index, type) ((type)table->boot->function[index])
extern SystemTable *platform_firmware(void);
static Guid tcp_binding = {0x00720665,0x67eb,0x4a99,{0xba,0xf7,0xd3,0xc3,0x3a,0x1c,0x7c,0xc9}};
static Guid tcp_protocol = {0x65530bc7,0xa359,0x410f,{0xb0,0x10,0x5a,0xad,0xc7,0xec,0x2b,0x62}};
static Guid ip_config = {0x5b446ed1,0xe30b,0x4faa,{0x87,0x1a,0x36,0x54,0xec,0xa3,0x60,0x80}};
typedef struct {
  int used, flags, close_exec, owner, state, error, eof, reading, writing;
  Binding *binding;
  Handle child;
  Tcp *tcp;
  Completion connect;
  IoToken receive, transmit;
  Packet receive_packet, transmit_packet;
  volatile int connected, received, transmitted;
  size_t available, offset;
  int64_t deadline;
  unsigned char read_buffer[SOCKET_BUFFER], write_buffer[SOCKET_BUFFER];
  struct sockaddr_in peer;
} Socket;
static Socket *sockets[SOCKET_COUNT];
int firmware_socket_descriptor(long descriptor) { return descriptor >= SOCKET_BASE && descriptor < SOCKET_BASE + SOCKET_COUNT; }
static int status_error(Status status) {
  if (!status) return 0;
  switch (status & ~(1ULL << 63)) {
    case 2: return EINVAL;
    case 3: return EOPNOTSUPP;
    case 6: case 17: return EAGAIN;
    case 9: return ENOMEM;
    case 18: return ETIMEDOUT;
    case 100: return ENETUNREACH;
    case 101: return EHOSTUNREACH;
    case 105: return ECONNRESET;
    case 106: return ECONNREFUSED;
    default: return EIO;
  }
}
static void UEFI_API completed(void *event, void *context) { (void)event; *(volatile int *)context = 1; }
static Status event(SystemTable *table, Completion *token, volatile int *done) {
  token->status = ERROR(6);
  return CALL(7,Status(UEFI_API *)(uint32_t,size_t,void(UEFI_API *)(void *,void *),void *,void **))(
      0x200,8,completed,(void *)done,&token->event);
}
static Socket *lookup(unsigned owner, long descriptor) {
  if (!firmware_socket_descriptor(descriptor)) return NULL;
  Socket *socket = sockets[descriptor-SOCKET_BASE];
  return socket && socket->used && socket->owner == (int)owner ? socket : NULL;
}
static int64_t milliseconds(void);
static Status configure_socket(Socket *socket) {
  TcpOptions options = {SOCKET_BUFFER,SOCKET_BUFFER,0,15,5,5,5,0,0,0,0,0,1,0,0};
  TcpConfig config = {0,64,{.automatic=1,.remote_port=ntohs(socket->peer.sin_port),.active=1},&options};
  memcpy(config.access.remote,&socket->peer.sin_addr,4);
  return socket->tcp->configure(socket->tcp,&config);
}
static void progress(Socket *socket) {
  if (!socket->tcp) return;
  socket->tcp->poll(socket->tcp);
  if (socket->state == 4) {
    Status status = configure_socket(socket);
    if (status == ERROR(17) && milliseconds() < socket->deadline) return;
    if (status == ERROR(17)) status = ERROR(18);
    if (!status) status = socket->tcp->connect(socket->tcp,&socket->connect);
    socket->error = status_error(status);
    socket->state = status ? 3 : 1;
  }
  if (socket->state == 1 && socket->connected) {
    socket->error = status_error(socket->connect.status);
    socket->state = socket->error ? 3 : 2;
  }
  if (socket->writing && socket->transmitted) {
    socket->writing = 0;
    if (socket->transmit.completion.status) { printf("SOCKET: transmit status=%llx\n", (unsigned long long)socket->transmit.completion.status); socket->error = status_error(socket->transmit.completion.status); }
  }
  if (socket->reading && socket->received) {
    socket->reading = 0;
    if (socket->receive.completion.status == ERROR(104)) socket->eof = 1;
    else if (socket->receive.completion.status) { printf("SOCKET: receive status=%llx\n", (unsigned long long)socket->receive.completion.status); socket->error = status_error(socket->receive.completion.status); }
    else {
      socket->available = socket->receive_packet.length;
      socket->offset = 0;
      if (socket->available > SOCKET_BUFFER) abort();
    }
  }
  if (socket->state == 2 && !socket->reading && !socket->available && !socket->eof && !socket->error) {
    socket->receive_packet = (Packet){0,0,SOCKET_BUFFER,1,{SOCKET_BUFFER,socket->read_buffer}};
    socket->receive.packet = &socket->receive_packet;
    socket->received = 0;
    Status status = socket->tcp->receive(socket->tcp,&socket->receive);
    if (status == ERROR(104)) socket->eof = 1;
    else if (status) { printf("SOCKET: receive enqueue status=%llx\n", (unsigned long long)status); socket->error = status_error(status); } else socket->reading = 1;
  }
}
static long close_socket(SystemTable *table, Socket *socket, long descriptor) {
  if (socket->tcp) {
    socket->tcp->cancel(socket->tcp,NULL);
    Status status = socket->tcp->configure(socket->tcp,NULL);
    if (status) return -status_error(status);
    /* Reset must release every firmware token before its backing storage is freed. */
    if ((socket->state == 1 && !socket->connected) ||
        (socket->reading && !socket->received) || (socket->writing && !socket->transmitted)) return -EBUSY;
  }
  if (socket->child) {
    Status status = socket->binding->destroy(socket->binding,socket->child);
    if (status) return -status_error(status);
  }
  for (Completion *token = &socket->connect; token;) {
    if (token->event) CALL(11,Status(UEFI_API *)(void *))(token->event);
    token = token == &socket->connect ? &socket->receive.completion :
            token == &socket->receive.completion ? &socket->transmit.completion : NULL;
  }
  sockets[descriptor-SOCKET_BASE] = NULL;
  free(socket);
  return 0;
}
void firmware_socket_reset(unsigned owner) {
  firmware_dns_reset(owner);
  SystemTable *table = platform_firmware();
  for (int index = 0; index < SOCKET_COUNT; ++index)
    if (sockets[index] && sockets[index]->owner == (int)owner)
      close_socket(table,sockets[index],SOCKET_BASE+index);
}
static long create_socket(SystemTable *table, unsigned owner, long domain, long type, long protocol) {
  if (domain != AF_INET) return -EAFNOSUPPORT;
  if ((type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) != SOCK_STREAM || (protocol && protocol != IPPROTO_TCP)) return -EPROTONOSUPPORT;
  int index;
  for (index = 0; index < SOCKET_COUNT && sockets[index]; ++index) {}
  if (index == SOCKET_COUNT) return -EMFILE;
  Socket *socket = calloc(1,sizeof(*socket));
  if (!socket) return -ENOMEM;
  socket->used = 1; socket->owner = owner; socket->flags = type & SOCK_NONBLOCK ? O_NONBLOCK : 0;
  socket->close_exec = !!(type & SOCK_CLOEXEC);
  sockets[index] = socket;
  return SOCKET_BASE+index;
}
static long connect_socket(SystemTable *table, Socket *socket, const struct sockaddr *address, size_t length) {
  if (!address || length < sizeof(struct sockaddr_in) || address->sa_family != AF_INET) return -EINVAL;
  if (socket->state) return socket->state == 2 ? -EISCONN : -EALREADY;
  if (socket->tcp) return -EALREADY;
  memcpy(&socket->peer,address,sizeof(socket->peer));
  Handle *handles = NULL; size_t count = 0;
  Status status = CALL(36,Status(UEFI_API *)(int,Guid *,void *,size_t *,Handle **))(2,&tcp_binding,NULL,&count,&handles);
  if (!status && !count) status = ERROR(14);
  if (!status) status = CALL(16,Status(UEFI_API *)(Handle,Guid *,void **))(handles[0],&tcp_binding,(void **)&socket->binding);
  if (!status) {
    void **config = NULL;
    if (!CALL(16,Status(UEFI_API *)(Handle,Guid *,void **))(handles[0],&ip_config,(void **)&config)) {
      uint32_t policy = 1;
      ((Status(UEFI_API *)(void *,uint32_t,size_t,void *))config[0])(config,1,sizeof(policy),&policy);
    }
  }
  if (handles) CALL(6,Status(UEFI_API *)(void *))(handles);
  if (!status) status = socket->binding->create(socket->binding,&socket->child);
  if (!status) status = CALL(16,Status(UEFI_API *)(Handle,Guid *,void **))(socket->child,&tcp_protocol,(void **)&socket->tcp);
  if (!status) status = event(table,&socket->connect,&socket->connected);
  if (!status) status = event(table,&socket->receive.completion,&socket->received);
  if (!status) status = event(table,&socket->transmit.completion,&socket->transmitted);
  if (!status) status = configure_socket(socket);
  if (status == ERROR(17)) { socket->state = 4; socket->deadline = milliseconds()+15000; return -EINPROGRESS; }
  if (!status) status = socket->tcp->connect(socket->tcp,&socket->connect);
  if (status) { printf("SOCKET: connect setup status=%llx child=%d protocol=%d\n",(unsigned long long)status,socket->child!=NULL,socket->tcp!=NULL); socket->error = status_error(status); socket->state = 3; return -socket->error; }
  socket->state = 1;
  return -EINPROGRESS;
}
long firmware_socket_service(unsigned owner, long number, const long arguments[6]) {
  SystemTable *table = platform_firmware();
  long descriptor = arguments[0], second = arguments[1], third = arguments[2], fourth = arguments[3];
  if (number == SYS_socket) return create_socket(table,owner,descriptor,second,third);
  if (number == SYS_poll) {
    struct pollfd *items = (void *)descriptor;
    if (second < 0 || second > SOCKET_COUNT || (second && !items)) return -EINVAL;
    int ready = 0;
    for (long index = 0; index < second; ++index) {
      struct pollfd *item = &items[index]; item->revents = 0;
      if (item->fd < 0) continue;
      Socket *socket = lookup(owner,item->fd);
      if (!socket) item->revents = POLLNVAL;
      else {
        progress(socket);
        if (socket->error) item->revents |= POLLERR;
        if (socket->eof || socket->state == 3) item->revents |= POLLHUP;
        if (socket->available || socket->eof) item->revents |= item->events & POLLIN;
        if (socket->state == 2 && !socket->writing && !socket->error) item->revents |= item->events & POLLOUT;
      }
      if (item->revents) ++ready;
    }
    return ready;
  }
  Socket *socket = lookup(owner,descriptor);
  if (!socket) return -EBADF;
  if (number == SYS_connect) return connect_socket(table,socket,(const struct sockaddr *)second,third);
  if (number == SYS_close) return close_socket(table,socket,descriptor);
  if (number == SYS_fcntl) {
    if (second == F_GETFL) return O_RDWR | socket->flags;
    if (second == F_SETFL) { socket->flags = third & O_NONBLOCK; return 0; }
    if (second == F_GETFD) return socket->close_exec ? FD_CLOEXEC : 0;
    if (second == F_SETFD) { socket->close_exec = !!(third & FD_CLOEXEC); return 0; }
    return -EINVAL;
  }
  progress(socket);
  if (number == SYS_getsockopt) {
    socklen_t *size = (void *)arguments[4];
    if (second != SOL_SOCKET || third != SO_ERROR) return -ENOPROTOOPT;
    if (!fourth || !size || *size < sizeof(int)) return -EINVAL;
    *(int *)fourth = socket->error; *size = sizeof(int); socket->error = 0;
    return 0;
  }
  if (number == SYS_setsockopt) return -ENOPROTOOPT;
  if (number == SYS_getpeername || number == SYS_getsockname) {
    socklen_t *size = (void *)third;
    if (!second || !size) return -EINVAL;
    if (socket->state != 2) return -ENOTCONN;
    struct sockaddr_in address = socket->peer;
    if (number == SYS_getsockname) {
      TcpConfig config = {0};
      Status status = socket->tcp->mode(socket->tcp,NULL,&config,NULL,NULL,NULL);
      if (status) return -status_error(status);
      memcpy(&address.sin_addr,config.access.address,4); address.sin_port = htons(config.access.port);
    }
    memcpy((void *)second,&address,*size < sizeof(address) ? *size : sizeof(address));
    *size = sizeof(address); return 0;
  }
  if (socket->error) return -socket->error;
  if (socket->state != 2) return (socket->state == 1 || socket->state == 4) ? -EAGAIN : -ENOTCONN;
  if (third < 0 || (third && !second)) return -EINVAL;
  if (number == SYS_read || number == SYS_recvfrom) {
    if (number == SYS_recvfrom && (fourth & ~(MSG_DONTWAIT | MSG_PEEK))) return -EOPNOTSUPP;
    if (number == SYS_recvfrom && (arguments[4] || arguments[5])) return -EOPNOTSUPP;
    if (!third) return 0;
    if (!socket->available) return socket->eof ? 0 : -EAGAIN;
    size_t size = (size_t)third < socket->available ? (size_t)third : socket->available;
    memcpy((void *)second,socket->read_buffer + socket->offset,size);
    if (number != SYS_recvfrom || !(fourth & MSG_PEEK)) { socket->offset += size; socket->available -= size; }
    return size;
  }
  if (number == SYS_write || number == SYS_sendto) {
    if (number == SYS_sendto && (fourth & ~(MSG_DONTWAIT | MSG_NOSIGNAL))) return -EOPNOTSUPP;
    if (number == SYS_sendto && (arguments[4] || arguments[5])) return -EISCONN;
    if (!third) return 0;
    if (socket->writing) return -EAGAIN;
    size_t size = third < SOCKET_BUFFER ? third : SOCKET_BUFFER;
    memcpy(socket->write_buffer,(void *)second,size);
    socket->transmit_packet = (Packet){1,0,size,1,{size,socket->write_buffer}};
    socket->transmit.packet = &socket->transmit_packet; socket->transmitted = 0;
    Status status = socket->tcp->transmit(socket->tcp,&socket->transmit);
    if (status) return -status_error(status);
    socket->writing = 1;
    return size;
  }
  return -EOPNOTSUPP;
}
static long dispatch(long number, const long arguments[6]) {
  if (renderer_runtime_is_child()) return (long)renderer_runtime_request(RENDERER_SOCKET_REQUEST,
      number,(uintptr_t)arguments,0,0,0);
  return firmware_socket_service(0,number,arguments);
}
static int64_t milliseconds(void) {
  struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
long firmware_socket_syscall(long number, long first, long second, long third,
                             long fourth, long fifth, long sixth) {
  long arguments[6] = {first,second,third,fourth,fifth,sixth};
  if (number == SYS_poll) {
    int64_t deadline = milliseconds() + (third < 0 ? INT_MAX : third);
    long result;
    do {
      result = dispatch(number,arguments);
      if (result || !third || milliseconds() >= deadline) return result;
      struct timespec delay = {0,1000000}; nanosleep(&delay,NULL);
    } while (1);
  }
  long result = dispatch(number,arguments);
  if (number != SYS_connect && number != SYS_read && number != SYS_write && number != SYS_sendto && number != SYS_recvfrom)
    return result;
  if (result != -EAGAIN && result != -EINPROGRESS) return result;
  long flags[6] = {first,F_GETFL,0,0,0,0};
  long mode = dispatch(SYS_fcntl,flags);
  if (mode < 0 || (mode & O_NONBLOCK) || ((number == SYS_sendto || number == SYS_recvfrom) && (fourth & MSG_DONTWAIT))) return result;
  struct pollfd ready = {first,(number == SYS_read || number == SYS_recvfrom) ? POLLIN : POLLOUT,0};
  long waited = firmware_socket_syscall(SYS_poll,(long)&ready,1,30000,0,0,0);
  if (waited <= 0) return waited ? waited : -ETIMEDOUT;
  if (number == SYS_connect) {
    int error; socklen_t size = sizeof(error);
    long option[6] = {first,SOL_SOCKET,SO_ERROR,(long)&error,(long)&size,0};
    result = dispatch(SYS_getsockopt,option);
    return result ? result : -error;
  }
  return dispatch(number,arguments);
}
