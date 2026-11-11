// Loads Android aarch64 shared objects from memory: maps PT_LOAD segments,
// applies RELA relocations and resolves imports through a caller-supplied
// resolver (bionic shims, other loaded modules, GL, host glibc).
#define _GNU_SOURCE
#include <elf.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "common.h"

#ifndef R_AARCH64_NONE
#define R_AARCH64_NONE 0
#endif

struct so_module {
  char *name;
  uintptr_t base;
  size_t span;
  Elf64_Phdr *phdr;
  int phnum;
  Elf64_Sym *dynsym;
  const char *dynstr;
  uint32_t nsyms;
  Elf64_Rela *rela, *jmprel;
  size_t relasz, jmprelsz;
  void (*init)(void);
  void (**init_array)(void);
  size_t init_count;
  struct so_module *next;
};

static so_module *mods;

static uint32_t sym_count(uintptr_t base, Elf64_Dyn *dyn) {
  for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++)
    if (d->d_tag == DT_HASH) return ((uint32_t *)(base + d->d_un.d_ptr))[1];
  for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
    if (d->d_tag != DT_GNU_HASH) continue;
    uint32_t *gh = (uint32_t *)(base + d->d_un.d_ptr);
    uint32_t nbuckets = gh[0], symoff = gh[1], bloom = gh[2];
    uint32_t *buckets = gh + 4 + bloom * 2;
    uint32_t *chain = buckets + nbuckets;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; i++) if (buckets[i] > last) last = buckets[i];
    if (last < symoff) return symoff;
    while (!(chain[last - symoff] & 1)) last++;
    return last + 1;
  }
  return 0;
}

so_module *so_load(const char *name, void *elf, size_t size, so_resolver resolve) {
  Elf64_Ehdr *eh = elf;
  if (size < sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_machine != EM_AARCH64) {
    LOG("%s: not an aarch64 ELF", name);
    return NULL;
  }
  Elf64_Phdr *ph = (Elf64_Phdr *)((char *)elf + eh->e_phoff);
  uintptr_t lo = UINTPTR_MAX, hi = 0;
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD) continue;
    if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
    if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
  }
  size_t pg = getpagesize();
  lo &= ~(pg - 1);
  hi = (hi + pg - 1) & ~(pg - 1);
  void *map = mmap(NULL, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map == MAP_FAILED) { LOG("%s: mmap failed", name); return NULL; }
  uintptr_t base = (uintptr_t)map - lo;
  Elf64_Dyn *dyn = NULL;
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD)
      memcpy((void *)(base + ph[i].p_vaddr), (char *)elf + ph[i].p_offset, ph[i].p_filesz);
    else if (ph[i].p_type == PT_DYNAMIC)
      dyn = (Elf64_Dyn *)(base + ph[i].p_vaddr);
  }
  if (!dyn) { LOG("%s: no PT_DYNAMIC", name); return NULL; }

  so_module *m = calloc(1, sizeof *m);
  m->name = strdup(name);
  m->base = base;
  m->span = hi - lo;
  m->phnum = eh->e_phnum;
  m->phdr = malloc(sizeof(Elf64_Phdr) * m->phnum);
  memcpy(m->phdr, ph, sizeof(Elf64_Phdr) * m->phnum);
  for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
    uintptr_t p = base + d->d_un.d_ptr;
    switch (d->d_tag) {
    case DT_SYMTAB: m->dynsym = (Elf64_Sym *)p; break;
    case DT_STRTAB: m->dynstr = (const char *)p; break;
    case DT_RELA: m->rela = (Elf64_Rela *)p; break;
    case DT_RELASZ: m->relasz = d->d_un.d_val; break;
    case DT_JMPREL: m->jmprel = (Elf64_Rela *)p; break;
    case DT_PLTRELSZ: m->jmprelsz = d->d_un.d_val; break;
    case DT_INIT: m->init = (void (*)(void))p; break;
    case DT_INIT_ARRAY: m->init_array = (void (**)(void))p; break;
    case DT_INIT_ARRAYSZ: m->init_count = d->d_un.d_val / sizeof(void *); break;
    }
  }
  m->nsyms = sym_count(base, dyn);
  // Register before relocating so self-references resolve through the module.
  m->next = mods;
  mods = m;

  int unresolved = 0;
  Elf64_Rela *tables[2] = { m->rela, m->jmprel };
  size_t sizes[2] = { m->relasz, m->jmprelsz };
  for (int t = 0; t < 2; t++) {
    for (size_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
      Elf64_Rela *r = &tables[t][i];
      uint32_t type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
      uint64_t *where = (uint64_t *)(base + r->r_offset);
      if (type == R_AARCH64_RELATIVE) { *where = base + r->r_addend; continue; }
      if (type == R_AARCH64_NONE) continue;
      if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT) {
        LOG("%s: unsupported relocation type %u", name, type);
        continue;
      }
      Elf64_Sym *s = &m->dynsym[si];
      const char *sn = m->dynstr + s->st_name;
      uintptr_t v = 0;
      void *ov = resolve(sn);
      if (ov) v = (uintptr_t)ov;
      else if (s->st_shndx != SHN_UNDEF) v = base + s->st_value;
      else {
        if (ELF64_ST_BIND(s->st_info) != STB_WEAK) {
          LOG("%s: unresolved import %s", name, sn);
          unresolved++;
        }
      }
      *where = v ? v + r->r_addend : 0;
    }
  }
  if (unresolved) LOG("%s: %d unresolved imports", name, unresolved);

  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD) continue;
    uintptr_t s = (base + ph[i].p_vaddr) & ~(pg - 1);
    uintptr_t e = (base + ph[i].p_vaddr + ph[i].p_memsz + pg - 1) & ~(pg - 1);
    int prot = (ph[i].p_flags & PF_R ? PROT_READ : 0) | (ph[i].p_flags & PF_W ? PROT_WRITE : 0) |
               (ph[i].p_flags & PF_X ? PROT_EXEC : 0);
    mprotect((void *)s, e - s, prot);
  }
  __builtin___clear_cache((char *)map, (char *)map + m->span);
  LOG("loaded %s at %p (%zu KB, %u syms)", name, (void *)map, m->span >> 10, m->nsyms);
  return m;
}

void so_run_init(so_module *m) {
  if (m->init) m->init();
  for (size_t i = 0; i < m->init_count; i++) {
    uintptr_t f = (uintptr_t)m->init_array[i];
    if (f && f != (uintptr_t)-1) ((void (*)(void))f)();
  }
}

static void *lookup(so_module *m, const char *name) {
  for (uint32_t i = 1; i < m->nsyms; i++) {
    Elf64_Sym *s = &m->dynsym[i];
    if (s->st_shndx == SHN_UNDEF || !s->st_name || ELF64_ST_BIND(s->st_info) == STB_LOCAL) continue;
    if (!strcmp(m->dynstr + s->st_name, name)) return (void *)(m->base + s->st_value);
  }
  return NULL;
}

void *so_sym(so_module *m, const char *name) { return lookup(m, name); }

// Search in load order (oldest first) like a global symbol scope.
static void *find_rec(so_module *m, const char *name) {
  if (!m) return NULL;
  void *r = find_rec(m->next, name);
  return r ? r : lookup(m, name);
}
void *so_find_global(const char *name) { return find_rec(mods, name); }

int so_addr_info(uintptr_t addr, const char **name, uintptr_t *off) {
  for (so_module *m = mods; m; m = m->next) {
    if (addr - m->base < m->span) { *name = m->name; *off = addr - m->base; return 1; }
  }
  return 0;
}

// dl_iterate_phdr for the Android modules first (the unwinders in libc++_shared
// and librobberybob find .eh_frame_hdr through it), then the host's objects.
int so_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
  for (so_module *m = mods; m; m = m->next) {
    struct dl_phdr_info info;
    memset(&info, 0, sizeof info);
    info.dlpi_addr = m->base;
    info.dlpi_name = m->name;
    info.dlpi_phdr = m->phdr;
    info.dlpi_phnum = m->phnum;
    int r = cb(&info, sizeof info, data);
    if (r) return r;
  }
  return dl_iterate_phdr(cb, data);
}
