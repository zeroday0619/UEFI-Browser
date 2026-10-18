#include <cerrno>
#include <cstddef>

// UEFI has no protection-key allocation service; callers must use page
// permissions.
int pkey_mprotect(void *, std::size_t, int, int) {
  errno = ENOTSUP;
  return -1;
}
int pkey_get(int) {
  errno = ENOTSUP;
  return -1;
}
int pkey_set(int, unsigned) {
  errno = ENOTSUP;
  return -1;
}
int pkey_alloc(unsigned, unsigned) {
  errno = ENOTSUP;
  return -1;
}
int pkey_free(int) {
  errno = ENOTSUP;
  return -1;
}
