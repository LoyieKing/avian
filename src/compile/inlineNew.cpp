/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_COMPILE_CPP_INCLUDE
#error "include this file from src/compile.cpp"
#endif

#include "compile/inlineNew.h"

#if TARGET_BYTES_PER_WORD == 8
// GcClass::vmFlags is a uint16 at this offset in the generated layout.
// Checked against a live class the first time a method is compiled.
static const unsigned ClassVmFlagsOffset = 10;

class InlineNewSlowPath {
 public:
  InlineNewSlowPath* next;
  Compiler::State* edge;
  unsigned slowIp;
  unsigned contIp;
  PoolElement* pool;
  TraceElement* trace;
};

// Opcode lengths for a conservative scan. jsr, jsr_w, wide, and the
// switch instructions fail the scan immediately: guessing their length
// could hide a jsr and collide side IPs with a subroutine copy.
bool inlineNewOpcodeLength(unsigned op, unsigned* len)
{
  if (op == jsr or op == jsr_w or op == wide or op == tableswitch
      or op == lookupswitch) {
    return false;
  }

  if (op <= 0x0f or (op >= 0x1a and op <= 0x35) or (op >= 0x3b and op <= 0x83)
      or (op >= 0x85 and op <= 0x98) or (op >= 0xac and op <= 0xb1)
      or op == 0xbe or op == 0xbf or op == 0xc2 or op == 0xc3) {
    *len = 1;
    return true;
  }

  if (op == 0x10 or op == 0x12 or (op >= 0x15 and op <= 0x19)
      or (op >= 0x36 and op <= 0x3a) or op == 0xa9 or op == 0xbc) {
    *len = 2;
    return true;
  }

  if (op == 0x11 or op == 0x13 or op == 0x14 or op == 0x84
      or (op >= 0x99 and op <= 0xa7) or (op >= 0xb2 and op <= 0xb8)
      or op == 0xbb or op == 0xbd or op == 0xc0 or op == 0xc1 or op == 0xc6
      or op == 0xc7) {
    *len = 3;
    return true;
  }

  if (op == 0xc5) {
    *len = 4;
    return true;
  }

  if (op == 0xb9 or op == 0xba or op == 0xc8) {
    *len = 5;
    return true;
  }

  return false;
}

bool codeAllowsInlineNew(GcCode* code)
{
  if (code == 0) {
    return false;
  }

  unsigned length = code->length();
  unsigned ip = 0;
  while (ip < length) {
    unsigned len = 0;
    unsigned op = static_cast<unsigned>(static_cast<uint8_t>(code->body()[ip]));
    if (not inlineNewOpcodeLength(op, &len) or len == 0 or ip + len > length) {
      return false;
    }
    ip += len;
  }
  return true;
}

bool methodAllowsInlineNew(Context* context)
{
  if (context->inlineNewOk < 0) {
    context->inlineNewOk = codeAllowsInlineNew(context->method->code()) ? 1 : 0;
  }
  return context->inlineNewOk == 1;
}

unsigned takeSideIp(Context* context)
{
  if (not context->sideIpsReady) {
    context->nextSideIp = context->method->code()->length();
    context->sideIpsReady = true;
  }

  unsigned ip = context->nextSideIp;
  ++context->nextSideIp;
  context->extendLogicalCode(1);
  return ip;
}

void queueInlineNew(Context* context, InlineNewSlowPath* path)
{
  path->next = 0;
  if (context->inlineNewSlowPathTail) {
    context->inlineNewSlowPathTail->next = path;
  } else {
    context->inlineNewSlowPaths = path;
  }
  context->inlineNewSlowPathTail = path;
}

// new_ is three bytes. LogicalInstruction::next falls into the next
// higher live index, so a block parked past the method is laid out after
// the return. The two unused bytes inside this new_ are free logical
// IPs: the bump and the join. The following bytecode then sits immediately
// after the join, and the slow call cannot be that fall-through.
bool inlineNewHolesFree(MyThread* t, Context* context, unsigned origin)
{
  GcCode* code = context->method->code();
  if (code == 0 or origin + 3 >= code->length()) {
    return false;
  }
  if (static_cast<unsigned>(static_cast<uint8_t>(code->body()[origin]))
      != new_) {
    return false;
  }
  if (context->visitTable[origin + 1] or context->visitTable[origin + 2]) {
    return false;
  }

  GcExceptionHandlerTable* eht = cast<GcExceptionHandlerTable>(
      t, code->exceptionHandlerTable());
  if (eht) {
    for (unsigned i = 0; i < eht->length(); ++i) {
      unsigned hip = exceptionHandlerIp(eht->body()[i]);
      if (hip == origin + 1 or hip == origin + 2) {
        return false;
      }
    }
  }
  return true;
}

// Fast path duplicates allocateSmall + setObjectClass. The slow path is
// the existing makeNew64 call, emitted after the walk. saveState's first
// target is the bump; the call fills the second. One branch covers class
// init, exclusive GC, and a full chunk, because a second condJump would
// need a third logical IP and the instruction only has two spare bytes.
bool InlineNew::tryCompile(MyThread* t,
                           Context* context,
                           Frame* frame,
                           GcClass* class_)
{
#ifdef VM_STRESS
  return false;
#endif

  if (context->bootContext or frame->subroutine) {
    return false;
  }

  if (class_->vmFlags() & (WeakReferenceFlag | HasFinalizerFlag)) {
    return false;
  }

  unsigned bytes = pad(class_->fixedSize());
  if (bytes == 0 or bytes > ThreadHeapSizeInBytes) {
    return false;
  }

  if (not methodAllowsInlineNew(context)) {
    return false;
  }

  if (not inlineNewHolesFree(t, context, frame->ip)) {
    return false;
  }

  PROTECT(t, class_);

  unsigned words = bytes / BytesPerWord;
  Compiler* c = context->compiler;

  TraceElement* trace = frame->trace(0, 0);
  ir::Value* classArg = frame->append(class_);

  PoolElement* pool = 0;
  for (PoolElement* e = context->objectPool; e; e = e->next) {
    if (e->target == class_) {
      pool = e;
      break;
    }
  }
  expect(t, pool);

  unsigned bumpIp = frame->ip + 1;
  unsigned contIp = frame->ip + 2;
  unsigned slowIp = takeSideIp(context);
  ir::Value* slow = c->promiseConstant(c->machineIp(slowIp), ir::Type::iptr());

  ir::Value* flags = c->load(
      ir::ExtendMode::Unsigned,
      c->memory(classArg, ir::Type::i2(), ClassVmFlagsOffset),
      ir::Type::iptr());
  ir::Value* needInit = c->binaryOp(lir::And,
                                    ir::Type::iptr(),
                                    c->constant(NeedInitFlag, ir::Type::iptr()),
                                    flags);
  ir::Value* machine = c->load(
      ir::ExtendMode::Signed,
      c->memory(c->threadRegister(), ir::Type::iptr(), ThreadMachineOffset),
      ir::Type::iptr());
  ir::Value* exclusive = c->load(
      ir::ExtendMode::Signed,
      c->memory(machine, ir::Type::iptr(), MachineExclusiveOffset),
      ir::Type::iptr());
  ir::Value* index = c->load(
      ir::ExtendMode::Unsigned,
      c->memory(c->threadRegister(), ir::Type::i4(), ThreadHeapIndexOffset),
      ir::Type::iptr());
  ir::Value* next = c->binaryOp(lir::Add,
                                ir::Type::iptr(),
                                c->constant(words, ir::Type::iptr()),
                                index);
  // subR is second minus first: ThreadHeapSizeInWords - next. Negative
  // exactly when next is strictly greater, which is allocate()'s test.
  ir::Value* limitMinusNext = c->binaryOp(
      lir::Subtract,
      ir::Type::iptr(),
      next,
      c->constant(ThreadHeapSizeInWords, ir::Type::iptr()));
  ir::Value* noRoom = c->binaryOp(lir::ShiftRight,
                                  ir::Type::iptr(),
                                  c->constant(63, ir::Type::iptr()),
                                  limitMinusNext);
  ir::Value* blocked
      = c->binaryOp(lir::Or, ir::Type::iptr(), needInit, exclusive);
  blocked = c->binaryOp(lir::Or, ir::Type::iptr(), blocked, noRoom);
  c->condJump(lir::JumpIfNotEqual,
              c->constant(0, ir::Type::iptr()),
              blocked,
              slow);
  Compiler::State* edge = c->saveState();
  c->startLogicalIp(bumpIp);

  // Values defined above a condJump lose their sites on the other side.
  // Reload the index and the class pointer here, on the fall-through.
  ir::Value* indexPtr = c->load(
      ir::ExtendMode::Unsigned,
      c->memory(c->threadRegister(), ir::Type::i4(), ThreadHeapIndexOffset),
      ir::Type::iptr());
  ir::Value* heap = c->load(
      ir::ExtendMode::Signed,
      c->memory(c->threadRegister(), ir::Type::iptr(), ThreadHeapOffset),
      ir::Type::iptr());
  ir::Value* scaled = c->binaryOp(lir::ShiftLeft,
                                  ir::Type::iptr(),
                                  c->constant(3, ir::Type::iptr()),
                                  indexPtr);
  ir::Value* objectPointer
      = c->binaryOp(lir::Add, ir::Type::iptr(), heap, scaled);

  ir::Value* storedNext
      = c->binaryOp(lir::Add,
                    ir::Type::i4(),
                    c->constant(words, ir::Type::i4()),
                    c->load(ir::ExtendMode::Unsigned,
                            c->memory(c->threadRegister(),
                                      ir::Type::i4(),
                                      ThreadHeapIndexOffset),
                            ir::Type::i4()));
  ir::Value* classBits = c->address(ir::Type::object(), pool);

  // The installed chunk is zeroed, so the header word is just the class.
  c->store(storedNext,
           c->memory(
               c->threadRegister(), ir::Type::i4(), ThreadHeapIndexOffset));
  c->store(classBits, c->memory(objectPointer, ir::Type::object(), 0));
  c->store(objectPointer,
           c->memory(c->threadRegister(),
                     ir::Type::iptr(),
                     AllocationResultOffset));

  c->startLogicalIp(contIp);
  frame->push(ir::Type::object(),
              c->load(ir::ExtendMode::Signed,
                      c->memory(c->threadRegister(),
                                ir::Type::object(),
                                AllocationResultOffset),
                      ir::Type::object()));

  InlineNewSlowPath* path = new (context->zone.allocate(sizeof(InlineNewSlowPath)))
      InlineNewSlowPath;
  path->edge = edge;
  path->slowIp = slowIp;
  path->contIp = contIp;
  path->pool = pool;
  path->trace = trace;
  queueInlineNew(context, path);
  return true;
}

void InlineNew::flush(MyThread* t, Context* context)
{
  Compiler* c = context->compiler;

  for (InlineNewSlowPath* path = context->inlineNewSlowPaths; path;
       path = path->next) {
    c->restoreState(path->edge);
    c->startLogicalIp(path->slowIp);

    ir::Value* slowClass = c->address(ir::Type::object(), path->pool);
    ir::Value* result = c->nativeCall(
        c->constant(getThunk(t, makeNew64Thunk), ir::Type::iptr()),
        0,
        path->trace,
        ir::Type::object(),
        args(c->threadRegister(), slowClass));
    c->store(result,
             c->memory(c->threadRegister(),
                       ir::Type::object(),
                       AllocationResultOffset));
    c->jmp(c->promiseConstant(c->machineIp(path->contIp), ir::Type::iptr()));
    // forkState is clear, so joining the load does not take a fork target.
    c->visitLogicalIp(path->contIp);
  }

  context->inlineNewSlowPaths = 0;
  context->inlineNewSlowPathTail = 0;
}

void InlineNew::checkClassVmFlagsOffset(MyThread* t, Context* context)
{
  GcClass* sample = context->method->class_();
  unsigned actual = static_cast<unsigned>(
      reinterpret_cast<uintptr_t>(&sample->vmFlags())
      - reinterpret_cast<uintptr_t>(sample));
  if (actual != ClassVmFlagsOffset) {
    fprintf(stderr,
            "constant mismatch (GcClass::vmFlags): \n\tconstant says: "
            "%d\n\tc++ compiler says: %d\n",
            static_cast<unsigned>(ClassVmFlagsOffset),
            actual);
    abort(t);
  }
}
#else  // not 64-bit target
bool InlineNew::tryCompile(MyThread* t UNUSED,
                           Context* context UNUSED,
                           Frame* frame UNUSED,
                           GcClass* class_ UNUSED)
{
  return false;
}

void InlineNew::flush(MyThread* t UNUSED, Context* context UNUSED)
{
}

void InlineNew::checkClassVmFlagsOffset(MyThread* t UNUSED, Context* context UNUSED)
{
}
#endif  // 64-bit target
