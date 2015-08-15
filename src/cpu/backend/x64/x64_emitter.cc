#include "cpu/backend/x64/x64_emitter.h"
#include "emu/profiler.h"

using namespace dreavm::core;
using namespace dreavm::cpu::backend::x64;
using namespace dreavm::cpu::ir;
using namespace dreavm::emu;

// maps register ids coming from IR values
static const Xbyak::Reg *reg_map_8[] = {&Xbyak::util::bl,
                                        &Xbyak::util::bpl,
                                        &Xbyak::util::r12b,
                                        &Xbyak::util::r13b,
                                        &Xbyak::util::r14b,
                                        &Xbyak::util::r15b,
                                        nullptr,
                                        nullptr,
                                        nullptr,
                                        nullptr,
                                        nullptr,
                                        nullptr,
                                        nullptr,
                                        nullptr};
static const Xbyak::Reg *reg_map_16[] = {&Xbyak::util::bx,
                                         &Xbyak::util::bp,
                                         &Xbyak::util::r12w,
                                         &Xbyak::util::r13w,
                                         &Xbyak::util::r14w,
                                         &Xbyak::util::r15w,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr};
static const Xbyak::Reg *reg_map_32[] = {
    &Xbyak::util::ebx,  &Xbyak::util::ebp,  &Xbyak::util::r12d,
    &Xbyak::util::r13d, &Xbyak::util::r14d, &Xbyak::util::r15d,
    &Xbyak::util::xmm2, &Xbyak::util::xmm3, &Xbyak::util::xmm4,
    &Xbyak::util::xmm5, &Xbyak::util::xmm6, &Xbyak::util::xmm7};
static const Xbyak::Reg *reg_map_64[] = {
    &Xbyak::util::rbx,  &Xbyak::util::rbp,  &Xbyak::util::r12,
    &Xbyak::util::r13,  &Xbyak::util::r14,  &Xbyak::util::r15,
    &Xbyak::util::xmm2, &Xbyak::util::xmm3, &Xbyak::util::xmm4,
    &Xbyak::util::xmm5, &Xbyak::util::xmm6, &Xbyak::util::xmm7};

// registers available for storing temporary values
static const Xbyak::Reg *tmp_regs_8[] = {&Xbyak::util::al, &Xbyak::util::cl,
                                         &Xbyak::util::dl};
static const Xbyak::Reg *tmp_regs_16[] = {&Xbyak::util::ax, &Xbyak::util::cx,
                                          &Xbyak::util::dx};
static const Xbyak::Reg *tmp_regs_32[] = {&Xbyak::util::eax, &Xbyak::util::ecx,
                                          &Xbyak::util::edx};
static const Xbyak::Reg *tmp_regs_64[] = {&Xbyak::util::rax, &Xbyak::util::rcx,
                                          &Xbyak::util::rdx};
static const Xbyak::Xmm *tmp_regs_xmm[] = {&Xbyak::util::xmm0,
                                           &Xbyak::util::xmm1};

// get and set Xbyak::Labels in the IR block's tag
#define GET_LABEL(blk) (*reinterpret_cast<Xbyak::Label *>(blk->tag()))
#define SET_LABEL(blk, lbl) (blk->set_tag(reinterpret_cast<intptr_t>(lbl)))

// callbacks for emitting each IR op
typedef void (*X64Emit)(X64Emitter &, Memory &, Xbyak::CodeGenerator &,
                        const Instr *);

static X64Emit x64_emitters[NUM_OPCODES];

#define EMITTER(op)                                               \
  void op(X64Emitter &e, Memory &memory, Xbyak::CodeGenerator &c, \
          const Instr *instr);                                    \
  static struct _x64_##op##_init {                                \
    _x64_##op##_init() { x64_emitters[OP_##op] = &op; }           \
  } x64_##op##_init;                                              \
  void op(X64Emitter &e, Memory &memory, Xbyak::CodeGenerator &c, \
          const Instr *instr)

X64Emitter::X64Emitter(Memory &memory, Xbyak::CodeGenerator &codegen)
    : memory_(memory), c_(codegen), arena_(1024) {}

X64Fn X64Emitter::Emit(IRBuilder &builder) {
  PROFILER_RUNTIME("X64Emitter::Emit");

  // getCurr returns the current spot in the codegen buffer which the function
  // is about to emitted to
  X64Fn fn = c_.getCurr<X64Fn>();

  // reset arena holding temporaries used while emitting
  arena_.Reset();

  // allocate the epilog label
  epilog_label_ = AllocLabel();

  // stack must be 16 byte aligned
  // TODO align each local
  int stack_size = 16 + builder.locals_size();
  // add 8 for function return value which will be pushed when this is called
  stack_size = align(stack_size, 16) + 8;
  assert((stack_size + 8) % 16 == 0);

  // emit prolog
  // FIXME only push registers that're used
  c_.push(Xbyak::util::rbx);
  c_.push(Xbyak::util::rbp);
  c_.push(Xbyak::util::r12);
  c_.push(Xbyak::util::r13);
  c_.push(Xbyak::util::r14);
  c_.push(Xbyak::util::r15);

  // reserve stack space for rdi copy
  c_.sub(Xbyak::util::rsp, stack_size);
  c_.mov(c_.qword[Xbyak::util::rsp + STACK_OFFSET_GUEST_CONTEXT],
         Xbyak::util::rdi);
  c_.mov(c_.qword[Xbyak::util::rsp + STACK_OFFSET_MEMORY], Xbyak::util::rsi);

  // generate labels for each block
  for (auto block : builder.blocks()) {
    Xbyak::Label *lbl = AllocLabel();
    SET_LABEL(block, lbl);
  }

  for (auto block : builder.blocks()) {
    c_.L(GET_LABEL(block));

    for (auto instr : block->instrs()) {
      ResetTmpRegisters();

      X64Emit emit = x64_emitters[instr->op()];
      CHECK(emit) << "Failed to find emitter for " << Opnames[instr->op()];
      emit(*this, memory_, c_, instr);
    }
  }

  // emit prolog
  c_.L(epilog_label());

  // reset stack
  c_.add(Xbyak::util::rsp, stack_size);

  // TODO only pop registers that're used
  c_.pop(Xbyak::util::r15);
  c_.pop(Xbyak::util::r14);
  c_.pop(Xbyak::util::r13);
  c_.pop(Xbyak::util::r12);
  c_.pop(Xbyak::util::rbp);
  c_.pop(Xbyak::util::rbx);

  c_.ret();

  c_.align(16);

  // patch up relocations
  c_.ready();

  // return the start of the buffer
  return fn;
}

Xbyak::Label *X64Emitter::AllocLabel() {
  Xbyak::Label *label = arena_.Alloc<Xbyak::Label>();
  new (label) Xbyak::Label();
  return label;
}

Xbyak::Address *X64Emitter::AllocAddress(const Xbyak::Address &from) {
  Xbyak::Address *addr = arena_.Alloc<Xbyak::Address>();
  new (addr) Xbyak::Address(from);
  return addr;
}

// Get the register / local allocated for the supplied value. If the value is
// a constant, copy it to a temporary register. The size argument can be
// overridden to get a truncated version of the value.
const Xbyak::Operand &X64Emitter::GetOperand(const Value *v, int size) {
  if (size == -1) {
    size = SizeForType(v->type());
  }

  if (v->reg() != NO_REGISTER) {
    const Xbyak::Reg *reg = nullptr;

    switch (size) {
      case 8:
        reg = reg_map_64[v->reg()];
        break;
      case 4:
        reg = reg_map_32[v->reg()];
        break;
      case 2:
        reg = reg_map_16[v->reg()];
        break;
      case 1:
        reg = reg_map_8[v->reg()];
        break;
    }

    CHECK_NOTNULL(reg);

    return *reg;
  } else if (v->local() != NO_SLOT) {
    Xbyak::Address *addr = nullptr;

    int offset = STACK_OFFSET_LOCALS + v->local();

    switch (size) {
      case 8:
        addr = AllocAddress(c_.qword[Xbyak::util::rsp + offset]);
        break;
      case 4:
        addr = AllocAddress(c_.dword[Xbyak::util::rsp + offset]);
        break;
      case 2:
        addr = AllocAddress(c_.word[Xbyak::util::rsp + offset]);
        break;
      case 1:
        addr = AllocAddress(c_.byte[Xbyak::util::rsp + offset]);
        break;
    }

    CHECK_NOTNULL(addr);

    return *addr;
  }

  // copy constant to tmp
  return GetTmpRegister(v, size);
}

// If the value is a local or constant, copy it to a tempory register, else
// return the register allocated for it.
const Xbyak::Reg &X64Emitter::GetRegister(const Value *v) {
  if (v->reg() == NO_REGISTER) {
    // copy local / constant to tmp
    return GetTmpRegister(v);
  }

  return reinterpret_cast<const Xbyak::Reg &>(GetOperand(v));
}

// If the prefered operand a register, copy the value to it and return, else do
// the regular GetRegister lookup.
const Xbyak::Reg &X64Emitter::GetPreferedRegister(
    const Value *v, const Xbyak::Operand &prefered) {
  if (prefered.isREG()) {
    CopyOperand(v, prefered);
    return reinterpret_cast<const Xbyak::Reg &>(prefered);
  }

  return GetRegister(v);
}

// If the value doesn't have a register allocated for it, return a temporary
// register without copying the value.
const Xbyak::Reg &X64Emitter::GetResultRegister(const Value *v) {
  if (v->reg() == NO_REGISTER) {
    return GetTmpRegister(nullptr, SizeForType(v->type()));
  }

  return reinterpret_cast<const Xbyak::Reg &>(GetOperand(v));
}

// Get a temporary register and copy value to it.
const Xbyak::Reg &X64Emitter::GetTmpRegister(const Value *v, int size) {
  static int num_tmp_regs = sizeof(tmp_regs_8) / sizeof(tmp_regs_8[0]);
  CHECK_LT(tmp_reg_, num_tmp_regs) << "Out of temporary registers";

  if (size == -1) {
    size = SizeForType(v->type());
  }

  const Xbyak::Reg *reg = nullptr;

  switch (size) {
    case 8:
      reg = tmp_regs_64[tmp_reg_];
      break;
    case 4:
      reg = tmp_regs_32[tmp_reg_];
      break;
    case 2:
      reg = tmp_regs_16[tmp_reg_];
      break;
    case 1:
      reg = tmp_regs_8[tmp_reg_];
      break;
  }

  CHECK_NOTNULL(reg);

  tmp_reg_++;

  // copy value to the temporary register
  if (v) {
    CopyOperand(v, *reg);
  }

  return *reg;
}

// Get the XMM register / local allocated for the supplied value. If the value
// is a constant, copy it to a temporary XMM register.
const Xbyak::Operand &X64Emitter::GetXMMOperand(const Value *v) {
  if (v->reg() == NO_REGISTER && v->local() == NO_SLOT) {
    // copy constant to tmp
    return GetTmpXMMRegister(v);
  }

  const Xbyak::Operand &op = GetOperand(v);
  CHECK(op.isXMM() || op.isMEM());
  return op;
}

// If the value isn't allocated a XMM register copy it to a temporary XMM,
// register, else return the XMM register allocated for it.
const Xbyak::Xmm &X64Emitter::GetXMMRegister(const Value *v) {
  const Xbyak::Operand &op = GetOperand(v);

  if (!op.isXMM()) {
    return GetTmpXMMRegister(v);
  }

  return reinterpret_cast<const Xbyak::Xmm &>(op);
}

// If the prefered operand is an XMM register, copy the value to it and return,
// else do the regular GetXMMRegister lookup.
const Xbyak::Xmm &X64Emitter::GetPreferedXMMRegister(
    const Value *v, const Xbyak::Operand &prefered) {
  if (prefered.isXMM()) {
    CopyOperand(v, prefered);
    return reinterpret_cast<const Xbyak::Xmm &>(prefered);
  }

  return GetXMMRegister(v);
}

// Get a temporary XMM register and copy value to it.
const Xbyak::Xmm &X64Emitter::GetTmpXMMRegister(const Value *v) {
  static int num_tmp_regs = sizeof(tmp_regs_xmm) / sizeof(tmp_regs_xmm[0]);
  CHECK_LT(tmp_xmm_reg_, num_tmp_regs) << "Out of temporary XMM registers";

  const Xbyak::Xmm *reg = tmp_regs_xmm[tmp_xmm_reg_++];

  // copy value to the temporary register
  if (v) {
    CopyOperand(v, *reg);
  }

  return *reg;
}

// Copy the value from src to dst if they're not the same operand. Note, this
// isn't fully generalized, it's just hackily handling the cases that've been
// needed so far.
const Xbyak::Operand &X64Emitter::CopyOperand(const Xbyak::Operand &from,
                                              const Xbyak::Operand &to) {
  if (from == to) {
    return to;
  }

  // shouldn't ever be doing this
  CHECK(!from.isREG() || !to.isREG() || from.getIdx() != to.getIdx() ||
        from.getKind() != to.getKind())
      << "Unexpected copy operation between the same register of different "
         "sizes";

  if (to.isXMM()) {
    if (from.isXMM()) {
      c_.movdqa(reinterpret_cast<const Xbyak::Xmm &>(to), from);
    } else if (from.isBit(32)) {
      c_.movss(reinterpret_cast<const Xbyak::Xmm &>(to), from);
    } else if (from.isBit(64)) {
      c_.movsd(reinterpret_cast<const Xbyak::Xmm &>(to), from);
    } else {
      LOG(FATAL) << "Unsupported copy";
    }
  } else if (from.isXMM()) {
    CHECK(to.isMEM()) << "Expected destination to be a memory address";

    if (to.isBit(32)) {
      c_.movss(reinterpret_cast<const Xbyak::Address &>(to),
               reinterpret_cast<const Xbyak::Xmm &>(from));
    } else if (to.isBit(64)) {
      c_.movsd(reinterpret_cast<const Xbyak::Address &>(to),
               reinterpret_cast<const Xbyak::Xmm &>(from));
    } else {
      LOG(FATAL) << "Unsupported copy";
    }
  } else if (to.isMEM() && from.isMEM()) {
    const Xbyak::Reg &tmp = GetTmpRegister(nullptr, from.getBit() >> 3);
    c_.mov(tmp, from);
    c_.mov(to, tmp);
    // FIXME could release tmp register now
  } else {
    c_.mov(to, from);
  }

  return to;
}

// Copy the value to the supplied operand.
const Xbyak::Operand &X64Emitter::CopyOperand(const Value *v,
                                              const Xbyak::Operand &to) {
  if (v->constant()) {
    if (to.isXMM()) {
      CHECK(IsFloatType(v->type()));

      if (v->type() == VALUE_F32) {
        float val = v->value<float>();
        c_.mov(c_.r8d, *reinterpret_cast<int32_t *>(&val));
        c_.movd(reinterpret_cast<const Xbyak::Xmm &>(to), c_.r8d);
      } else {
        double val = v->value<double>();
        c_.mov(c_.r8, *reinterpret_cast<int64_t *>(&val));
        c_.movq(reinterpret_cast<const Xbyak::Xmm &>(to), c_.r8);
      }
    } else {
      CHECK(IsIntType(v->type()));

      c_.mov(to, v->GetZExtValue());
    }
  } else {
    const Xbyak::Operand &from = GetOperand(v);

    CopyOperand(from, to);
  }

  return to;
}

bool X64Emitter::CanEncodeAsImmediate(const Value *v) const {
  if (!v->constant()) {
    return false;
  }

  return v->type() <= VALUE_I32;
}

void X64Emitter::RestoreParameters() {
  c_.mov(Xbyak::util::rdi,
         c_.qword[Xbyak::util::rsp + STACK_OFFSET_GUEST_CONTEXT]);
  c_.mov(Xbyak::util::rsi, c_.qword[Xbyak::util::rsp + STACK_OFFSET_MEMORY]);
}

void X64Emitter::ResetTmpRegisters() {
  tmp_reg_ = 0;
  tmp_xmm_reg_ = 0;
}

EMITTER(LOAD_CONTEXT) {
  int offset = instr->arg0()->value<int32_t>();
  int result_sz = SizeForType(instr->result()->type());
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (result.isXMM()) {
    const Xbyak::Xmm &result_xmm = reinterpret_cast<const Xbyak::Xmm &>(result);

    if (result_sz == 4) {
      c.movss(result_xmm, c.dword[c.rdi + offset]);
    } else if (result_sz == 8) {
      c.movsd(result_xmm, c.qword[c.rdi + offset]);
    }
  } else {
    // dst must be a register, mov doesn't support mem -> mem
    const Xbyak::Reg &tmp = e.GetResultRegister(instr->result());

    if (result_sz == 1) {
      c.mov(tmp, c.byte[c.rdi + offset]);
    } else if (result_sz == 2) {
      c.mov(tmp, c.word[c.rdi + offset]);
    } else if (result_sz == 4) {
      c.mov(tmp, c.dword[c.rdi + offset]);
    } else if (result_sz == 8) {
      c.mov(tmp, c.qword[c.rdi + offset]);
    }

    e.CopyOperand(tmp, result);
  }
}

EMITTER(STORE_CONTEXT) {
  int offset = instr->arg0()->value<int32_t>();
  int data_sz = SizeForType(instr->arg1()->type());

  if (instr->arg1()->constant()) {
    if (data_sz == 1) {
      c.mov(c.byte[c.rdi + offset], instr->arg1()->value<int8_t>());
    } else if (data_sz == 2) {
      c.mov(c.word[c.rdi + offset], instr->arg1()->value<int16_t>());
    } else if (data_sz == 4) {
      c.mov(c.dword[c.rdi + offset], instr->arg1()->value<int32_t>());
    } else if (data_sz == 8) {
      c.mov(c.qword[c.rdi + offset], instr->arg1()->value<int64_t>());
    }
  } else {
    const Xbyak::Operand &src = e.GetOperand(instr->arg1());

    if (src.isXMM()) {
      const Xbyak::Xmm &src_xmm = reinterpret_cast<const Xbyak::Xmm &>(src);

      if (data_sz == 4) {
        c.movss(c.dword[c.rdi + offset], src_xmm);
      } else if (data_sz == 8) {
        c.movsd(c.qword[c.rdi + offset], src_xmm);
      }
    } else {
      // src must come from a register, mov doesn't support mem -> mem
      const Xbyak::Reg &src_reg = e.GetRegister(instr->arg1());

      if (data_sz == 1) {
        c.mov(c.byte[c.rdi + offset], src_reg);
      } else if (data_sz == 2) {
        c.mov(c.word[c.rdi + offset], src_reg);
      } else if (data_sz == 4) {
        c.mov(c.dword[c.rdi + offset], src_reg);
      } else if (data_sz == 8) {
        c.mov(c.qword[c.rdi + offset], src_reg);
      }
    }
  }
}

EMITTER(LOAD_LOCAL) {
  int offset = STACK_OFFSET_LOCALS + instr->arg0()->value<int32_t>();

  if (IsFloatType(instr->result()->type())) {
    const Xbyak::Xmm &result = e.GetXMMRegister(instr->result());

    switch (instr->result()->type()) {
      case VALUE_F32:
        c.movss(result, c.dword[c.rsp + offset]);
        break;
      case VALUE_F64:
        c.movsd(result, c.qword[c.rsp + offset]);
        break;
      default:
        LOG(FATAL) << "Unexpected result type";
        break;
    }
  } else {
    const Xbyak::Reg &result = e.GetRegister(instr->result());

    switch (instr->result()->type()) {
      case VALUE_I8:
        c.mov(result, c.byte[c.rsp + offset]);
        break;
      case VALUE_I16:
        c.mov(result, c.word[c.rsp + offset]);
        break;
      case VALUE_I32:
        c.mov(result, c.dword[c.rsp + offset]);
        break;
      case VALUE_I64:
        c.mov(result, c.qword[c.rsp + offset]);
        break;
      default:
        LOG(FATAL) << "Unexpected result type";
        break;
    }
  }
}

EMITTER(STORE_LOCAL) {
  int offset = STACK_OFFSET_LOCALS + instr->arg0()->value<int32_t>();

  CHECK(!instr->arg1()->constant());

  if (IsFloatType(instr->arg1()->type())) {
    const Xbyak::Xmm &src = e.GetXMMRegister(instr->arg1());

    switch (instr->arg1()->type()) {
      case VALUE_F32:
        c.movss(c.dword[c.rsp + offset], src);
        break;
      case VALUE_F64:
        c.movsd(c.qword[c.rsp + offset], src);
        break;
      default:
        LOG(FATAL) << "Unexpected value type";
        break;
    }
  } else {
    const Xbyak::Reg &src = e.GetRegister(instr->arg1());

    switch (instr->arg1()->type()) {
      case VALUE_I8:
        c.mov(c.byte[c.rsp + offset], src);
        break;
      case VALUE_I16:
        c.mov(c.word[c.rsp + offset], src);
        break;
      case VALUE_I32:
        c.mov(c.dword[c.rsp + offset], src);
        break;
      case VALUE_I64:
        c.mov(c.qword[c.rsp + offset], src);
        break;
      default:
        LOG(FATAL) << "Unexpected value type";
        break;
    }
  }
}

EMITTER(LOAD) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (instr->arg0()->constant()) {
    // try to resolve the address to a physical page
    int32_t addr = instr->arg0()->value<int32_t>();
    MemoryBank *bank = nullptr;
    uint32_t offset = 0;

    memory.Resolve(addr, &bank, &offset);

    // if the address maps to a physical page, not a dynamic handler, let's
    // make it fast
    if (bank->physical_addr) {
      const Xbyak::Reg &tmp = e.GetResultRegister(instr->result());

      // FIXME it'd be nice if xbyak had a mov operation which would convert
      // the displacement to a RIP-relative address when finalizing code so
      // we didn't have to store the absolute address in the scratch register
      void *physical_addr = bank->physical_addr + offset;
      c.mov(c.r8, (size_t)physical_addr);

      switch (instr->result()->type()) {
        case VALUE_I8:
          c.mov(tmp, c.byte[c.r8]);
          break;
        case VALUE_I16:
          c.mov(tmp, c.word[c.r8]);
          break;
        case VALUE_I32:
          c.mov(tmp, c.dword[c.r8]);
          break;
        case VALUE_I64:
          c.mov(tmp, c.qword[c.r8]);
          break;
        default:
          LOG(FATAL) << "Unsupported load result type";
          break;
      }

      e.CopyOperand(tmp, result);

      return;
    }
  }

  void *fn = nullptr;
  switch (instr->result()->type()) {
    case VALUE_I8:
      fn = reinterpret_cast<void *>(
          static_cast<uint8_t (*)(Memory *, uint32_t)>(&Memory::R8));
      break;
    case VALUE_I16:
      fn = reinterpret_cast<void *>(
          static_cast<uint16_t (*)(Memory *, uint32_t)>(&Memory::R16));
      break;
    case VALUE_I32:
      fn = reinterpret_cast<void *>(
          static_cast<uint32_t (*)(Memory *, uint32_t)>(&Memory::R32));
      break;
    case VALUE_I64:
      fn = reinterpret_cast<void *>(
          static_cast<uint64_t (*)(Memory *, uint32_t)>(&Memory::R64));
      break;
    default:
      LOG(FATAL) << "Unsupported load result type";
      break;
  }

  // setup arguments
  c.mov(c.rdi, c.rsi);
  e.CopyOperand(instr->arg0(), c.rsi);

  // call func
  c.mov(c.rax, (uintptr_t)fn);
  c.call(c.rax);

  // copy off result
  c.mov(result, c.rax);

  e.RestoreParameters();
}

EMITTER(STORE) {
  if (instr->arg0()->constant()) {
    // try to resolve the address to a physical page
    int32_t addr = instr->arg0()->value<int32_t>();
    MemoryBank *bank = nullptr;
    uint32_t offset = 0;

    memory.Resolve(addr, &bank, &offset);

    // if the address maps to a physical page, not a dynamic handler, let's
    // make it fast
    if (bank->physical_addr) {
      const Xbyak::Reg &b = e.GetRegister(instr->arg1());

      // FIXME it'd be nice if xbyak had a mov operation which would convert
      // the displacement to a RIP-relative address when finalizing code so
      // we didn't have to store the absolute address in the scratch register
      void *physical_addr = bank->physical_addr + offset;
      c.mov(c.r8, (size_t)physical_addr);

      switch (instr->arg1()->type()) {
        case VALUE_I8:
          c.mov(c.byte[c.r8], b);
          break;
        case VALUE_I16:
          c.mov(c.word[c.r8], b);
          break;
        case VALUE_I32:
          c.mov(c.dword[c.r8], b);
          break;
        case VALUE_I64:
          c.mov(c.qword[c.r8], b);
          break;
        default:
          LOG(FATAL) << "Unsupported store value type";
          break;
      }

      return;
    }
  }

  void *fn = nullptr;
  switch (instr->arg1()->type()) {
    case VALUE_I8:
      fn = reinterpret_cast<void *>(
          static_cast<void (*)(Memory *, uint32_t, uint8_t)>(&Memory::W8));
      break;
    case VALUE_I16:
      fn = reinterpret_cast<void *>(
          static_cast<void (*)(Memory *, uint32_t, uint16_t)>(&Memory::W16));
      break;
    case VALUE_I32:
      fn = reinterpret_cast<void *>(
          static_cast<void (*)(Memory *, uint32_t, uint32_t)>(&Memory::W32));
      break;
    case VALUE_I64:
      fn = reinterpret_cast<void *>(
          static_cast<void (*)(Memory *, uint32_t, uint64_t)>(&Memory::W64));
      break;
    default:
      LOG(FATAL) << "Unsupported store value type";
      break;
  }

  // setup arguments
  c.mov(c.rdi, c.rsi);
  e.CopyOperand(instr->arg0(), c.rsi);
  e.CopyOperand(instr->arg1(), c.rdx);

  // call func
  c.mov(c.rax, (uintptr_t)fn);
  c.call(c.rax);

  e.RestoreParameters();
}

EMITTER(CAST) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Operand &a = e.GetOperand(instr->arg0());

  switch (instr->result()->type()) {
    case VALUE_I32:
      CHECK_EQ(instr->arg0()->type(), VALUE_F32);
      c.cvttss2si(result, a);
      break;
    case VALUE_I64:
      CHECK_EQ(instr->arg0()->type(), VALUE_F64);
      c.cvttsd2si(result, a);
      break;
    case VALUE_F32:
      CHECK_EQ(instr->arg0()->type(), VALUE_I32);
      c.cvtsi2ss(result, a);
      break;
    case VALUE_F64:
      CHECK_EQ(instr->arg0()->type(), VALUE_I64);
      c.cvtsi2sd(result, a);
      break;
    default:
      CHECK(false);
      break;
  }
}

EMITTER(SEXT) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Operand &a = e.GetOperand(instr->arg0());

  if (a == result) {
    // already the correct width
    return;
  }

  const Xbyak::Reg &tmp = e.GetResultRegister(instr->result());

  if (result.isBit(64) && a.isBit(32)) {
    c.movsxd(tmp.cvt64(), a);
  } else {
    c.movsx(tmp, a);
  }

  e.CopyOperand(tmp, result);
}

EMITTER(ZEXT) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Operand &a = e.GetOperand(instr->arg0());

  if (a == result) {
    // already the correct width
    return;
  }

  const Xbyak::Reg &tmp = e.GetResultRegister(instr->result());

  if (result.isBit(64)) {
    // mov will automatically zero fill the upper 32-bits
    c.mov(tmp.cvt32(), a);
  } else {
    c.movzx(tmp, a);
  }

  e.CopyOperand(tmp, result);
}

EMITTER(TRUNCATE) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Operand &a = e.GetOperand(instr->arg0());

  if (a == result) {
    // already the correct width
    return;
  }

  const Xbyak::Reg &tmp = e.GetResultRegister(instr->result());
  const Xbyak::Operand &truncated =
      e.GetOperand(instr->arg0(), result.getBit() >> 3);

  if (truncated.isBit(32)) {
    // mov will automatically zero fill the upper 32-bits
    c.mov(tmp, truncated);
  } else {
    c.movzx(tmp.cvt32(), truncated);
  }

  e.CopyOperand(tmp, result);
}

EMITTER(SELECT) {
  const Xbyak::Reg &cond = e.GetRegister(instr->arg0());
  c.test(cond, cond);

  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Operand &a = e.GetOperand(instr->arg1());
  const Xbyak::Operand &b = e.GetOperand(instr->arg2());
  const Xbyak::Reg &tmp = e.GetResultRegister(instr->result());

  c.cmovnz(tmp.cvt32(), a);
  c.cmovz(tmp.cvt32(), b);

  e.CopyOperand(tmp, result);
}

EMITTER(EQ) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (IsFloatType(instr->arg0()->type())) {
    const Xbyak::Xmm &a = e.GetXMMRegister(instr->arg0());
    const Xbyak::Operand &b = e.GetXMMOperand(instr->arg1());

    if (instr->arg0()->type() == VALUE_F32) {
      c.comiss(a, b);
    } else {
      c.comisd(a, b);
    }
  } else {
    const Xbyak::Operand &a = e.GetOperand(instr->arg0());

    if (e.CanEncodeAsImmediate(instr->arg1())) {
      c.cmp(a, (uint32_t)instr->arg1()->GetZExtValue());
    } else {
      const Xbyak::Operand &b = e.GetOperand(instr->arg1());
      c.cmp(a, b);
    }
  }

  c.sete(result);
}

EMITTER(NE) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (IsFloatType(instr->arg0()->type())) {
    const Xbyak::Xmm &a = e.GetXMMRegister(instr->arg0());
    const Xbyak::Operand &b = e.GetXMMOperand(instr->arg1());

    if (instr->arg0()->type() == VALUE_F32) {
      c.comiss(a, b);
    } else {
      c.comisd(a, b);
    }
  } else {
    const Xbyak::Operand &a = e.GetOperand(instr->arg0());

    if (e.CanEncodeAsImmediate(instr->arg1())) {
      c.cmp(a, (uint32_t)instr->arg1()->GetZExtValue());
    } else {
      const Xbyak::Operand &b = e.GetOperand(instr->arg1());
      c.cmp(a, b);
    }
  }

  c.setne(result);
}

EMITTER(SGE) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (IsFloatType(instr->arg0()->type())) {
    const Xbyak::Xmm &a = e.GetXMMRegister(instr->arg0());
    const Xbyak::Operand &b = e.GetXMMOperand(instr->arg1());

    if (instr->arg0()->type() == VALUE_F32) {
      c.comiss(a, b);
    } else {
      c.comisd(a, b);
    }

    c.setae(result);
  } else {
    const Xbyak::Operand &a = e.GetOperand(instr->arg0());

    if (e.CanEncodeAsImmediate(instr->arg1())) {
      c.cmp(a, (uint32_t)instr->arg1()->GetZExtValue());
    } else {
      const Xbyak::Operand &b = e.GetOperand(instr->arg1());
      c.cmp(a, b);
    }

    c.setge(result);
  }
}

EMITTER(SGT) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (IsFloatType(instr->arg0()->type())) {
    const Xbyak::Xmm &a = e.GetXMMRegister(instr->arg0());
    const Xbyak::Operand &b = e.GetXMMOperand(instr->arg1());

    if (instr->arg0()->type() == VALUE_F32) {
      c.comiss(a, b);
    } else {
      c.comisd(a, b);
    }

    c.seta(result);
  } else {
    const Xbyak::Operand &a = e.GetOperand(instr->arg0());

    if (e.CanEncodeAsImmediate(instr->arg1())) {
      c.cmp(a, (uint32_t)instr->arg1()->GetZExtValue());
    } else {
      const Xbyak::Operand &b = e.GetOperand(instr->arg1());
      c.cmp(a, b);
    }

    c.setg(result);
  }
}

EMITTER(UGE) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Operand &a = e.GetOperand(instr->arg0());

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    c.cmp(a, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Operand &b = e.GetOperand(instr->arg1());
    c.cmp(a, b);
  }

  c.setae(result);
}

EMITTER(UGT) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Operand &a = e.GetOperand(instr->arg0());

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    c.cmp(a, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Operand &b = e.GetOperand(instr->arg1());
    c.cmp(a, b);
  }

  c.seta(result);
}

EMITTER(SLE) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (IsFloatType(instr->arg0()->type())) {
    const Xbyak::Xmm &a = e.GetXMMRegister(instr->arg0());
    const Xbyak::Operand &b = e.GetXMMOperand(instr->arg1());

    if (instr->arg0()->type() == VALUE_F32) {
      c.comiss(a, b);
    } else {
      c.comisd(a, b);
    }

    c.setbe(result);
  } else {
    const Xbyak::Operand &a = e.GetOperand(instr->arg0());

    if (e.CanEncodeAsImmediate(instr->arg1())) {
      c.cmp(a, (uint32_t)instr->arg1()->GetZExtValue());
    } else {
      const Xbyak::Operand &b = e.GetOperand(instr->arg1());
      c.cmp(a, b);
    }

    c.setle(result);
  }
}

EMITTER(SLT) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (IsFloatType(instr->arg0()->type())) {
    const Xbyak::Xmm &a = e.GetXMMRegister(instr->arg0());
    const Xbyak::Operand &b = e.GetXMMOperand(instr->arg1());

    if (instr->arg0()->type() == VALUE_F32) {
      c.comiss(a, b);
    } else {
      c.comisd(a, b);
    }

    c.setb(result);
  } else {
    const Xbyak::Operand &a = e.GetOperand(instr->arg0());

    if (e.CanEncodeAsImmediate(instr->arg1())) {
      c.cmp(a, (uint32_t)instr->arg1()->GetZExtValue());
    } else {
      const Xbyak::Operand &b = e.GetOperand(instr->arg1());
      c.cmp(a, b);
    }

    c.setl(result);
  }
}

EMITTER(ULE) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Operand &a = e.GetOperand(instr->arg0());

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    c.cmp(a, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Operand &b = e.GetOperand(instr->arg1());
    c.cmp(a, b);
  }

  c.setbe(result);
}

EMITTER(ULT) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Operand &a = e.GetOperand(instr->arg0());

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    c.cmp(a, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Operand &b = e.GetOperand(instr->arg1());
    c.cmp(a, b);
  }

  c.setb(result);
}

EMITTER(ADD) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (IsFloatType(instr->result()->type())) {
    const Xbyak::Xmm &a = e.GetPreferedXMMRegister(instr->arg0(), result);
    const Xbyak::Operand &b = e.GetXMMOperand(instr->arg1());

    if (instr->result()->type() == VALUE_F32) {
      c.addss(a, b);
    } else {
      c.addsd(a, b);
    }

    e.CopyOperand(a, result);
  } else {
    e.CopyOperand(instr->arg0(), result);

    if (e.CanEncodeAsImmediate(instr->arg1())) {
      c.add(result, (uint32_t)instr->arg1()->GetZExtValue());
    } else {
      const Xbyak::Operand &b = e.GetOperand(instr->arg1());
      c.add(result, b);
    }
  }
}

EMITTER(SUB) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (IsFloatType(instr->result()->type())) {
    const Xbyak::Xmm &a = e.GetPreferedXMMRegister(instr->arg0(), result);
    const Xbyak::Operand &b = e.GetXMMOperand(instr->arg1());

    if (instr->result()->type() == VALUE_F32) {
      c.subss(a, b);
    } else {
      c.subsd(a, b);
    }

    e.CopyOperand(a, result);
  } else {
    e.CopyOperand(instr->arg0(), result);

    if (e.CanEncodeAsImmediate(instr->arg1())) {
      c.sub(result, (uint32_t)instr->arg1()->GetZExtValue());
    } else {
      const Xbyak::Operand &b = e.GetOperand(instr->arg1());
      c.sub(result, b);
    }
  }
}

EMITTER(SMUL) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (IsFloatType(instr->result()->type())) {
    const Xbyak::Xmm &a = e.GetPreferedXMMRegister(instr->arg0(), result);
    const Xbyak::Operand &b = e.GetXMMOperand(instr->arg1());

    if (instr->result()->type() == VALUE_F32) {
      c.mulss(a, b);
    } else {
      c.mulsd(a, b);
    }

    e.CopyOperand(a, result);
  } else {
    const Xbyak::Reg &a = e.GetPreferedRegister(instr->arg0(), result);
    const Xbyak::Operand &b = e.GetOperand(instr->arg1());

    c.imul(a, b);

    e.CopyOperand(a, result);
  }
}

EMITTER(UMUL) {
  CHECK(IsIntType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Reg &a = e.GetPreferedRegister(instr->arg0(), result);
  const Xbyak::Operand &b = e.GetOperand(instr->arg1());

  c.imul(a, b);

  e.CopyOperand(a, result);
}

EMITTER(DIV) {
  CHECK(IsFloatType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Xmm &a = e.GetPreferedXMMRegister(instr->arg0(), result);
  const Xbyak::Operand &b = e.GetXMMOperand(instr->arg1());

  if (instr->result()->type() == VALUE_F32) {
    c.divss(a, b);
  } else {
    c.divsd(a, b);
  }

  e.CopyOperand(a, result);
}

EMITTER(NEG) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (IsFloatType(instr->result()->type())) {
    const Xbyak::Xmm &a = e.GetPreferedXMMRegister(instr->arg0(), result);

    if (instr->result()->type() == VALUE_F32) {
      // TODO use xorps
      c.movd(c.eax, a);
      c.mov(c.ecx, (uint32_t)0x80000000);
      c.xor (c.eax, c.ecx);
      if (result.isXMM()) {
        c.movd(reinterpret_cast<const Xbyak::Xmm &>(result), c.eax);
      } else {
        c.mov(result, c.eax);
      }
    } else {
      // TODO use xorpd
      c.movq(c.rax, a);
      c.mov(c.rcx, (uint64_t)0x8000000000000000);
      c.xor (c.rax, c.rcx);
      if (result.isXMM()) {
        c.movq(reinterpret_cast<const Xbyak::Xmm &>(result), c.rax);
      } else {
        c.mov(result, c.rax);
      }
    }
  } else {
    e.CopyOperand(instr->arg0(), result);

    c.neg(result);
  }
}

EMITTER(SQRT) {
  CHECK(IsFloatType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());
  const Xbyak::Operand &a = e.GetXMMOperand(instr->arg0());
  const Xbyak::Xmm &tmp = result.isXMM()
                              ? reinterpret_cast<const Xbyak::Xmm &>(result)
                              : e.GetTmpXMMRegister();

  if (instr->result()->type() == VALUE_F32) {
    c.sqrtss(tmp, a);
  } else {
    c.sqrtsd(tmp, a);
  }

  e.CopyOperand(tmp, result);
}

EMITTER(ABS) {
  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (IsFloatType(instr->result()->type())) {
    const Xbyak::Xmm &a = e.GetPreferedXMMRegister(instr->arg0(), result);

    if (instr->result()->type() == VALUE_F32) {
      // TODO use andps
      c.movd(c.eax, a);
      c.mov(c.ecx, (uint32_t)0x7fffffff);
      c.and (c.eax, c.ecx);
      if (result.isXMM()) {
        c.movd(reinterpret_cast<const Xbyak::Xmm &>(result), c.eax);
      } else {
        c.mov(result, c.eax);
      }
    } else {
      // TODO use andpd
      c.movq(c.rax, a);
      c.mov(c.rcx, (uint64_t)0x7fffffffffffffff);
      c.and (c.rax, c.rcx);
      if (result.isXMM()) {
        c.movq(reinterpret_cast<const Xbyak::Xmm &>(result), c.rax);
      } else {
        c.mov(result, c.rax);
      }
    }
  } else {
    LOG(FATAL) << "Verify this works";
    // c.mov(c.rax, *result);
    // c.neg(c.rax);
    // c.cmovl(reinterpret_cast<const Xbyak::Reg *>(result)->cvt32(), c.rax);
  }
}

EMITTER(SIN) {
  CHECK(IsFloatType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (instr->result()->type() == VALUE_F32) {
    e.CopyOperand(instr->arg0(), c.xmm0);
    c.mov(c.rax, (uint64_t)&sinf);
    c.call(c.rax);
    if (result.isXMM()) {
      c.movss(reinterpret_cast<const Xbyak::Xmm &>(result), c.xmm0);
    } else {
      c.movss(reinterpret_cast<const Xbyak::Address &>(result), c.xmm0);
    }
  } else {
    e.CopyOperand(instr->arg0(), c.xmm0);
    c.mov(c.rax, (uint64_t)&sin);
    c.call(c.rax);
    if (result.isXMM()) {
      c.movsd(reinterpret_cast<const Xbyak::Xmm &>(result), c.xmm0);
    } else {
      c.movsd(reinterpret_cast<const Xbyak::Address &>(result), c.xmm0);
    }
  }

  e.RestoreParameters();
}

EMITTER(COS) {
  CHECK(IsFloatType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());

  if (instr->result()->type() == VALUE_F32) {
    e.CopyOperand(instr->arg0(), c.xmm0);
    c.mov(c.rax, (uint64_t)&cosf);
    c.call(c.rax);
    if (result.isXMM()) {
      c.movss(reinterpret_cast<const Xbyak::Xmm &>(result), c.xmm0);
    } else {
      c.movss(reinterpret_cast<const Xbyak::Address &>(result), c.xmm0);
    }
  } else {
    e.CopyOperand(instr->arg0(), c.xmm0);
    c.mov(c.rax, (uint64_t)&cos);
    c.call(c.rax);
    if (result.isXMM()) {
      c.movsd(reinterpret_cast<const Xbyak::Xmm &>(result), c.xmm0);
    } else {
      c.movsd(reinterpret_cast<const Xbyak::Address &>(result), c.xmm0);
    }
  }

  e.RestoreParameters();
}

EMITTER(AND) {
  CHECK(IsIntType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());

  e.CopyOperand(instr->arg0(), result);

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    c.and (result, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Operand &b = e.GetOperand(instr->arg1());
    c.and (result, b);
  }
}

EMITTER(OR) {
  CHECK(IsIntType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());

  e.CopyOperand(instr->arg0(), result);

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    c.or (result, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Operand &b = e.GetOperand(instr->arg1());
    c.or (result, b);
  }
}

EMITTER(XOR) {
  CHECK(IsIntType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());

  e.CopyOperand(instr->arg0(), result);

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    c.xor (result, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Operand &b = e.GetOperand(instr->arg1());
    c.xor (result, b);
  }
}

EMITTER(NOT) {
  CHECK(IsIntType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());

  e.CopyOperand(instr->arg0(), result);

  c.not(result);
}

EMITTER(SHL) {
  CHECK(IsIntType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());

  e.CopyOperand(instr->arg0(), result);

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    c.shl(result, (int)instr->arg1()->GetZExtValue());
  } else {
    // // cl is a tmp reg, this could blow up otherwise
    // CHECK_EQ(tmp_reg_, 0);

    e.CopyOperand(instr->arg1(), c.cl);
    c.shl(result, c.cl);
  }
}

EMITTER(ASHR) {
  CHECK(IsIntType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());

  e.CopyOperand(instr->arg0(), result);

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    c.sar(result, (int)instr->arg1()->GetZExtValue());
  } else {
    // // cl is a tmp reg, this could blow up otherwise
    // CHECK_EQ(tmp_reg_, 0);

    e.CopyOperand(instr->arg1(), c.cl);
    c.sar(result, c.cl);
  }
}

EMITTER(LSHR) {
  CHECK(IsIntType(instr->result()->type()));

  const Xbyak::Operand &result = e.GetOperand(instr->result());

  e.CopyOperand(instr->arg0(), result);

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    c.shr(result, (int)instr->arg1()->GetZExtValue());
  } else {
    // // cl is a tmp reg, this could blow up otherwise
    // CHECK_EQ(tmp_reg_, 0);

    e.CopyOperand(instr->arg1(), c.cl);
    c.shr(result, c.cl);
  }
}

EMITTER(BRANCH) {
  if (instr->arg0()->type() == VALUE_BLOCK) {
    // jump to local block
    Block *dst = instr->arg0()->value<Block *>();
    c.jmp(GET_LABEL(dst), Xbyak::CodeGenerator::T_NEAR);
  } else {
    // return if we need to branch to a far block
    e.CopyOperand(instr->arg0(), c.rax);
    c.jmp(e.epilog_label());
  }
}

EMITTER(BRANCH_COND) {
  const Xbyak::Reg &cond = e.GetRegister(instr->arg0());

  c.test(cond, cond);

  // if both blocks are a local block this is easy
  if (instr->arg1()->type() == VALUE_BLOCK &&
      instr->arg2()->type() == VALUE_BLOCK) {
    // jump to local block
    const Block *next_block = instr->block()->next();
    Block *block_true = instr->arg1()->value<Block *>();
    Block *block_false = instr->arg2()->value<Block *>();

    // don't emit a jump if the block is next
    if (next_block != block_true) {
      c.jnz(GET_LABEL(block_true), Xbyak::CodeGenerator::T_NEAR);
    }
    if (next_block != block_false) {
      c.je(GET_LABEL(block_false), Xbyak::CodeGenerator::T_NEAR);
    }
  }
  // if both blocks are a far block this is easy
  else if (instr->arg1()->type() != VALUE_BLOCK &&
           instr->arg2()->type() != VALUE_BLOCK) {
    // return if we need to branch to a far block
    const Xbyak::Operand &op_true = e.GetOperand(instr->arg1());
    const Xbyak::Operand &op_false = e.GetOperand(instr->arg2());

    c.mov(c.eax, op_true);
    c.jnz(e.epilog_label());

    c.mov(c.eax, op_false);
    c.je(e.epilog_label());
  }
  // if they are mixed, do local block test first, far block second
  else {
    LOG(FATAL) << "Unsupported mixed mode conditional branch";
  }
}

EMITTER(CALL_EXTERNAL) {
  // rdi is already pointing to guest_ctx
  uint64_t addr = instr->arg0()->GetZExtValue();
  c.mov(c.rax, addr);
  c.call(c.rax);

  e.RestoreParameters();
}
