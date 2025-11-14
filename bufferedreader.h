#pragma once
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "types.h"

// need this typing because cpp mindlessly promotes
// bool to int. so it thinks statements like
// { (bool)foo < 0 } are sensical even though it will never happen
enum class read_t { OK, ERR };
inline bool is_ok(read_t const retcode) { return read_t::OK == retcode; }
inline read_t operator!(read_t const code)
{
  return code;  // will throw compiler error if you try to use it like boolean!
}

/** Struct for buffered reading. Everything is public to give low level access
 * if needed */
typedef struct buf {
  buf(int fd): fd(fd) {
   struct stat sb;
   fstat(fd, &sb);
   limit = sb.st_size;
   ptr = (char *) mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  }
  ~buf() { munmap( ptr, limit ); }

  char *ptr;

  uint64_t limit; // size of mapped file
  uint64_t pos;
  fd_t fd = -1;

  /** Returns a pointer to ptr + pos + idx */
  char const *get(unsigned int idx) const
  {
    return ptr + pos + idx;
  }

  /** Returns available bytes in buf */
  unsigned available(void) const { return limit - pos; }
  bool available(unsigned n) const
  {
    return pos + n <= limit;  // faster than available() >= n;
  }
  void advance(unsigned bytes)
  {
    pos += bytes;
    assert(pos <= limit);
  }
  /* blocking read. blocks until 1 or more (until available()) bytes are
   * available.
   */
  ssize_t read(void);
  /* blocking read. blocks until at least n bytes are available
   * returns number of bytes read. 0 in case of EOF and -1 for other read error.
   */
  ssize_t read(unsigned n);
  /* Essentially a wrapper around read(n). If at least n bytes are
   * available in the buffer returns true immediately. Otherwise
   * it tries to read as many bytes as necessary to get n bytes
   * in the buffer. Returns false if read(n) returns <= 0.
   */
  read_t ensure(unsigned n) { return pos+n <= limit ? read_t::OK : read_t::ERR; }
} buf_t;
