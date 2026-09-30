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

#include "compile/youngObjectStore.h"

#if TARGET_BYTES_PER_WORD == 8
// takeSideIp and the jsr scan live with InlineNew. A subroutine copy
// occupies the next length-sized window of logical IPs, so this pass
// refuses the same methods the bump refuses.
bool methodAllowsInlineNew(Context* context);
unsigned takeSideIp(Context* context);
bool inTryBlock(MyThread* t, GcCode* code, unsigned ip);

// Slow path of a reference store whose fast path is the plain store.
// markOnly writes and then calls markField. Otherwise the call is
// setMaybeNull, which still throws on a null receiver. Flushed after
// the walk so the call is not the fall-through of the next bytecode.
class YoungObjectStoreSlowPath {
 public:
  YoungObjectStoreSlowPath* next;
  Compiler::State* edge;
  unsigned slowIp;
  unsigned contIp;
  TraceElement* trace;
  ir::Value* self;
  ir::Value* value;
  int offset;
  bool markOnly;
};

void queueYoungObjectStore(Context* context, YoungObjectStoreSlowPath* path)
{
  path->next = 0;
  if (context->youngStoreSlowPathTail) {
    context->youngStoreSlowPathTail->next = path;
  } else {
    context->youngStoreSlowPaths = path;
  }
  context->youngStoreSlowPathTail = path;
}

// A three-byte instruction. The operand bytes are free logical IPs,
// same layout as inlined new_: the store falls through, and the native
// call lives on a side IP that jumps back. putfield's holes are its
// index bytes; invokespecial's are the two bytes already used this way.
bool objectStoreHolesFree(MyThread* t,
                          Context* context,
                          unsigned origin,
                          unsigned opcode)
{
  GcCode* code = context->method->code();
  if (code == 0 or origin + 3 >= code->length()) {
    return false;
  }
  if (static_cast<unsigned>(static_cast<uint8_t>(code->body()[origin]))
      != opcode) {
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

// The TLAB is a slice of eden, not of gen2, and the object is not a
// fixie, so a pointer inside [heap, heapEnd) does not need a
// remembered-set update. Null, tenured, and fixed objects fail the
// range test. One condJump: the instruction only has two spare bytes.
// The end moves when the TLAB is resized, so this is a compare rather
// than a constant shift.
bool YoungObjectStore::tryCompile(MyThread* t,
                                  Frame* frame,
                                  unsigned callIp,
                                  ir::Value* self,
                                  ir::Value* value,
                                  int offset,
                                  unsigned opcode,
                                  bool markOnly)
{
  Context* context = frame->context;
  if (callIp != frame->ip or frame->subroutine) {
    return false;
  }
  // The slow call is a side IP past the bytecode. Handler ranges do not
  // cover it, so a null receiver inside a try would escape the catch.
  // The inline setMaybeNull at this ip stays inside the range.
  if (inTryBlock(t, context->method->code(), callIp)) {
    return false;
  }
  if (not methodAllowsInlineNew(context)) {
    return false;
  }
  if (not objectStoreHolesFree(t, context, callIp, opcode)) {
    return false;
  }

  Compiler* c = context->compiler;
  TraceElement* trace = frame->trace(0, 0);

  unsigned fastIp = callIp + 1;
  unsigned contIp = callIp + 2;
  unsigned slowIp = takeSideIp(context);
  ir::Value* slow = c->promiseConstant(c->machineIp(slowIp), ir::Type::iptr());

  ir::Value* heap = c->load(
      ir::ExtendMode::Signed,
      c->memory(c->threadRegister(), ir::Type::iptr(), ThreadHeapOffset),
      ir::Type::iptr());
  ir::Value* end = c->load(
      ir::ExtendMode::Signed,
      c->memory(c->threadRegister(), ir::Type::iptr(), ThreadHeapEndOffset),
      ir::Type::iptr());
  // subR is second minus first. User pointers sit below 2^63, so the
  // sign bit is the compare. self < heap, or self >= end, is outside.
  // A null chunk has heap == end == 0, and end - self - 1 is negative.
  ir::Value* selfMinusHeap
      = c->binaryOp(lir::Subtract, ir::Type::iptr(), heap, self);
  ir::Value* below = c->binaryOp(lir::ShiftRight,
                                 ir::Type::iptr(),
                                 c->constant(63, ir::Type::iptr()),
                                 selfMinusHeap);
  ir::Value* endMinusSelf
      = c->binaryOp(lir::Subtract, ir::Type::iptr(), self, end);
  ir::Value* endMinusSelfMinusOne = c->binaryOp(
      lir::Subtract, ir::Type::iptr(), c->constant(1, ir::Type::iptr()), endMinusSelf);
  ir::Value* above = c->binaryOp(lir::ShiftRight,
                                 ir::Type::iptr(),
                                 c->constant(63, ir::Type::iptr()),
                                 endMinusSelfMinusOne);
  ir::Value* outside
      = c->binaryOp(lir::Or, ir::Type::iptr(), below, above);
  c->condJump(lir::JumpIfNotEqual,
              c->constant(0, ir::Type::iptr()),
              outside,
              slow);
  Compiler::State* edge = c->saveState();
  c->startLogicalIp(fastIp);

  // self and value are stack homes. The range math is only a branch
  // input; recomputing it here would be a value with no site.
  c->store(value, c->memory(self, ir::Type::object(), offset));

  c->startLogicalIp(contIp);

  YoungObjectStoreSlowPath* path = new (
      context->zone.allocate(sizeof(YoungObjectStoreSlowPath)))
      YoungObjectStoreSlowPath;
  path->edge = edge;
  path->slowIp = slowIp;
  path->contIp = contIp;
  path->trace = trace;
  path->self = self;
  path->value = value;
  path->offset = offset;
  path->markOnly = markOnly;
  queueYoungObjectStore(context, path);
  return true;
}

void YoungObjectStore::flush(MyThread* t, Context* context)
{
  Compiler* c = context->compiler;

  for (YoungObjectStoreSlowPath* path = context->youngStoreSlowPaths; path;
       path = path->next) {
    c->restoreState(path->edge);
    c->startLogicalIp(path->slowIp);

    if (path->markOnly) {
      // The receiver is not null. Store, then mark: targetNeedsMark
      // reads the new referent. mark does not collect on this path.
      // A skip-zero object is safe here because the field is written
      // before the call. The other path cannot store first: the
      // receiver may be null, and setMaybeNull is what throws.
      c->store(path->value,
               c->memory(path->self, ir::Type::object(), path->offset));
      c->nativeCall(
          c->constant(getThunk(t, markFieldThunk), ir::Type::iptr()),
          0,
          0,
          ir::Type::void_(),
          args(c->threadRegister(),
               path->self,
               c->constant(path->offset, ir::Type::i4())));
    } else {
      c->nativeCall(
          c->constant(getThunk(t, setMaybeNullThunk), ir::Type::iptr()),
          0,
          path->trace,
          ir::Type::void_(),
          args(c->threadRegister(),
               path->self,
               c->constant(path->offset, ir::Type::i4()),
               path->value));
    }
    c->jmp(c->promiseConstant(c->machineIp(path->contIp), ir::Type::iptr()));
    c->visitLogicalIp(path->contIp);
  }

  context->youngStoreSlowPaths = 0;
  context->youngStoreSlowPathTail = 0;
}
#else  // not 64-bit target
bool YoungObjectStore::tryCompile(MyThread* t UNUSED,
                                  Frame* frame UNUSED,
                                  unsigned callIp UNUSED,
                                  ir::Value* self UNUSED,
                                  ir::Value* value UNUSED,
                                  int offset UNUSED,
                                  unsigned opcode UNUSED,
                                  bool markOnly UNUSED)
{
  return false;
}

void YoungObjectStore::flush(MyThread* t UNUSED, Context* context UNUSED)
{
}
#endif  // 64-bit target
