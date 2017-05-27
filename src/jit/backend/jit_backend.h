#ifndef JIT_BACKEND_H
#define JIT_BACKEND_H

#include <stdint.h>

struct exception;
struct ir;
struct jit;
struct jit_block;

struct jit_register {
  const char *name;
  int value_types;
  const void *data;
};

struct jit_backend {
  struct jit *jit;

  const struct jit_register *registers;
  int num_registers;

  void (*init)(struct jit_backend *);
  void (*destroy)(struct jit_backend *);

  /* compile interface */
  void (*reset)(struct jit_backend *);
  int (*assemble_code)(struct jit_backend *, struct jit_block *, struct ir *);
  void (*dump_code)(struct jit_backend *, const uint8_t *, int);
  int (*handle_exception)(struct jit_backend *, struct exception *);

  /* dispatch interface */
  void (*run_code)(struct jit_backend *, int);
  void *(*lookup_code)(struct jit_backend *, uint32_t);
  void (*cache_code)(struct jit_backend *, uint32_t, void *);
  void (*invalidate_code)(struct jit_backend *, uint32_t);
  void (*patch_edge)(struct jit_backend *, void *, void *);
  void (*restore_edge)(struct jit_backend *, void *, uint32_t);
};

#endif
