// ocarina-ps4 (2026-11-08): o __expand_heap do musl do SDK, com o crescimento limitado.
//
// O original (libc.a, expand_heap.lo) não tem brk no PS4: toda expansão do heap é um mmap anônimo de no mínimo
// 16 KiB << (mmap_step / 2), e mmap_step sobe a cada expansão. Depois de ~28 expansões, um malloc pequeno faz o heap pedir
// 256 MiB de uma vez, depois 512 MiB; e memória do heap o musl nunca devolve. No OoT isso esgotou a memória direta
// (pedidos de 256/512 MiB vindos de um realloc de 64 KiB). Aqui o mínimo para em ORBIS_HEAP_STEP_MAX (8 MiB): o resto do
// comportamento é o mesmo (página de 16 KiB, mesmo mmap, que o orbis_mmap atende dos carve-outs).
// O orbis-compat entra no link inteiro e antes da libc, então este __expand_heap é o usado e o expand_heap.lo da libc
// não é puxado.
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef ORBIS_HEAP_STEP_MAX
#define ORBIS_HEAP_STEP_MAX (8u << 20)
#endif

void *__mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);

void *__expand_heap(size_t *pn) {
  static unsigned mmap_step;
  size_t n = *pn;
  if (n >= 0x7fffffffffffc000ull) {
    errno = ENOMEM;
    return 0;
  }
  n += -n & 0x3fff;
  unsigned shift = mmap_step / 2;
  size_t min = shift >= 20 ? (size_t)ORBIS_HEAP_STEP_MAX : ((size_t)0x4000 << shift);
  if (min > ORBIS_HEAP_STEP_MAX) min = ORBIS_HEAP_STEP_MAX;
  if (n < min) n = min;
  void *area = __mmap(0, n, 3 /* PROT_READ|PROT_WRITE */, 0x1002 /* MAP_PRIVATE|MAP_ANON */, -1, 0);
  if (area == (void *)-1) return 0;
  *pn = n;
  mmap_step++;
  return area;
}
