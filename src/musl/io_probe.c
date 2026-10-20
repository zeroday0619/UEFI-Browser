#define _GNU_SOURCE
#include "renderer_runtime.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#define VERIFY(test) do { if (!(test)) { dprintf(1,"FAIL: I/O probe line=%u errno=%d\n",__LINE__,errno); return 1; } } while (0)
int run_firmware_io_probe(void) {
  int child=renderer_runtime_is_child();
  if (child) {
    char byte;
    errno=0;VERIFY(read(64,&byte,1)==-1 && errno==EBADF);
    errno=0;VERIFY(read(1024,&byte,1)==-1 && errno==EBADF);
    dprintf(1,"IO PROBE: renderer cannot use browser-owned descriptors\n");
  }
  const char *path=child ? "/profile/renderer-probe.dat" : "/profile/browser-probe.dat";
  int descriptor=open(path,O_CREAT|O_RDWR|O_CLOEXEC,0600);
  VERIFY(descriptor>=0);
  unsigned previous=0;
  ssize_t length=read(descriptor,&previous,sizeof(previous));
  VERIFY(length==0 || length==sizeof(previous));
  unsigned generation=previous+1;
  VERIFY(lseek(descriptor,0,SEEK_SET)==0);
  VERIFY(write(descriptor,&generation,sizeof(generation))==sizeof(generation));
  VERIFY(fsync(descriptor)==0);
  struct stat info;
  VERIFY(fstat(descriptor,&info)==0 && S_ISREG(info.st_mode) && info.st_size==sizeof(generation));
  VERIFY(close(descriptor)==0);
  descriptor=open(path,O_RDONLY); VERIFY(descriptor>=0);
  unsigned actual=0; VERIFY(read(descriptor,&actual,sizeof(actual))==sizeof(actual) && actual==generation);
  VERIFY(close(descriptor)==0);
  errno=0; VERIFY(open("/profile/../escape",O_CREAT|O_RDWR,0600)==-1 && errno==EINVAL);
  errno=0; VERIFY(open("/EFI/BOOT/BOOTX64.EFI",O_RDONLY)==-1 && errno==EACCES);
  descriptor=open("/profile/transient-probe.dat",O_CREAT|O_RDWR|O_TRUNC,0600); VERIFY(descriptor>=0);
  const char bytes[]={'a',0,'b',0xff};
  VERIFY(write(descriptor,bytes,sizeof(bytes))==sizeof(bytes));
  VERIFY(ftruncate(descriptor,2)==0 && lseek(descriptor,0,SEEK_END)==2);
  VERIFY(close(descriptor)==0 && unlink("/profile/transient-probe.dat")==0);
  dprintf(1,"IO PROBE: file_generation=%u owner=%d\n",generation,child);
  descriptor=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,IPPROTO_TCP); VERIFY(descriptor>=0);
  struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(8765)};
  VERIFY(inet_pton(AF_INET,"10.0.2.2",&address.sin_addr)==1);
  int connected=connect(descriptor,(void *)&address,sizeof(address));
  VERIFY(connected==0 || (connected==-1 && errno==EINPROGRESS));
  struct pollfd ready={descriptor,POLLOUT,0};
  VERIFY(poll(&ready,1,20000)==1 && (ready.revents&POLLOUT));
  int error=-1;socklen_t size=sizeof(error);
  VERIFY(getsockopt(descriptor,SOL_SOCKET,SO_ERROR,&error,&size)==0 && !error);
  struct sockaddr_in peer;size=sizeof(peer);
  VERIFY(getpeername(descriptor,(void *)&peer,&size)==0 && peer.sin_port==address.sin_port);
  const char request[]="POST /network/echo HTTP/1.1\r\nHost: 10.0.2.2:8765\r\nContent-Length: 4\r\nConnection: close\r\n\r\na\0b\xff";
  VERIFY(send(descriptor,request,sizeof(request)-1,MSG_NOSIGNAL)==sizeof(request)-1);
  char response[4096];size_t used=0;
  for (;;) {
    ready.events=POLLIN;ready.revents=0;
    VERIFY(poll(&ready,1,20000)==1 && (ready.revents&(POLLIN|POLLHUP)));
    ssize_t received=recv(descriptor,response+used,sizeof(response)-1-used,0);
    if (received<0 && errno==EAGAIN) continue;
    VERIFY(received>=0);
    if (!received) break;
    used+=received;VERIFY(used<sizeof(response)-1);
  }
  response[used]=0;
  VERIFY(strstr(response,"200 OK") && strstr(response,"610062ff"));
  VERIFY(close(descriptor)==0);
  errno=0;VERIFY(read(descriptor,response,1)==-1 && errno==EBADF);
  dprintf(1,"IO PROBE: TCP nonblocking connect, binary send, poll, receive and EOF passed owner=%d\n",child);
  if (!child) {
    VERIFY(open("/profile/browser-owned.dat",O_CREAT|O_RDWR,0600)==64);
    VERIFY(socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0)==1024);
  }
  return 0;
}
