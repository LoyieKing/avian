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

#include "compile/objectStore.h"
#include "compile/youngObjectStore.h"

// Zero when this ip has no agreed fact, including a null vector.
// Subroutine copies share the bytecode ip: frame->ip is that ip, not
// the duplicated logical address.
uint8_t referenceStoreFact(Context* context, unsigned ip)
{
  uint8_t* facts = context->objectStoreFacts;
  GcCode* code = context->method ? context->method->code() : 0;
  if (facts == 0 or code == 0 or ip >= code->length()) {
    return 0;
  }
  uint8_t fact = facts[ip];
  if (fact == ObjectStoreFacts::Fresh or fact == ObjectStoreFacts::NonNull) {
    return fact;
  }
  return 0;
}

void plainReferenceStore(avian::codegen::Compiler* c,
                         ir::Value* object,
                         ir::Value* value,
                         int offset)
{
  c->store(value, c->memory(object, ir::Type::object(), offset));
}

void markReferenceStore(MyThread* t,
                        avian::codegen::Compiler* c,
                        ir::Value* object,
                        ir::Value* value,
                        int offset)
{
  plainReferenceStore(c, object, value, offset);
  c->nativeCall(c->constant(getThunk(t, markFieldThunk), ir::Type::iptr()),
                0,
                0,
                ir::Type::void_(),
                args(c->threadRegister(),
                     object,
                     c->constant(offset, ir::Type::i4())));
}

void maybeNullReferenceStore(MyThread* t,
                             Frame* frame,
                             avian::codegen::Compiler* c,
                             ir::Value* object,
                             ir::Value* value,
                             int offset)
{
  c->nativeCall(c->constant(getThunk(t, setMaybeNullThunk), ir::Type::iptr()),
                0,
                frame->trace(0, 0),
                ir::Type::void_(),
                args(c->threadRegister(),
                     object,
                     c->constant(offset, ir::Type::i4()),
                     value));
}

void ObjectStore::store(MyThread* t,
                        Frame* frame,
                        unsigned ip,
                        ir::Value* object,
                        ir::Value* value,
                        int offset,
                        unsigned holeOpcode,
                        bool allowBranch)
{
  avian::codegen::Compiler* c = frame->c;
  uint8_t fact = referenceStoreFact(frame->context, ip);

  // needsMark is false for the whole life of this value so far. No
  // null check and no remembered-set update.
  if (fact == ObjectStoreFacts::Fresh) {
    plainReferenceStore(c, object, value, offset);
    return;
  }

  // One condJump. The fast path falls through into the plain store.
  // The slow path is queued and must jump to the join.
  if (allowBranch
      and YoungObjectStore::tryCompile(t,
                                       frame,
                                       ip,
                                       object,
                                       value,
                                       offset,
                                       holeOpcode,
                                       fact == ObjectStoreFacts::NonNull)) {
    return;
  }

  if (fact == ObjectStoreFacts::NonNull) {
    markReferenceStore(t, c, object, value, offset);
    return;
  }

  maybeNullReferenceStore(t, frame, c, object, value, offset);
}

void ObjectStore::storeElement(MyThread* t,
                               Frame* frame,
                               unsigned ip,
                               ir::Value* array,
                               ir::Value* index,
                               ir::Value* value)
{
  avian::codegen::Compiler* c = frame->c;
  uint8_t fact = referenceStoreFact(frame->context, ip);

  if (fact == ObjectStoreFacts::Fresh) {
    c->store(value,
             c->memory(array, ir::Type::object(), TargetArrayBody, index));
    return;
  }

  if (fact == ObjectStoreFacts::NonNull) {
    // Same byte offset setMaybeNull is given: the body plus the scaled
    // index. mark divides that by the word size.
    ir::Value* byteOffset = c->binaryOp(
        lir::Add,
        ir::Type::i4(),
        c->constant(TargetArrayBody, ir::Type::i4()),
        c->binaryOp(lir::ShiftLeft,
                    ir::Type::i4(),
                    c->constant(log(TargetBytesPerWord), ir::Type::i4()),
                    index));
    c->store(value,
             c->memory(array, ir::Type::object(), TargetArrayBody, index));
    c->nativeCall(c->constant(getThunk(t, markFieldThunk), ir::Type::iptr()),
                  0,
                  0,
                  ir::Type::void_(),
                  args(c->threadRegister(), array, byteOffset));
    return;
  }

  c->nativeCall(
      c->constant(getThunk(t, setMaybeNullThunk), ir::Type::iptr()),
      0,
      frame->trace(0, 0),
      ir::Type::void_(),
      args(c->threadRegister(),
           array,
           c->binaryOp(lir::Add,
                       ir::Type::i4(),
                       c->constant(TargetArrayBody, ir::Type::i4()),
                       c->binaryOp(lir::ShiftLeft,
                                   ir::Type::i4(),
                                   c->constant(log(TargetBytesPerWord),
                                               ir::Type::i4()),
                                   index)),
           value));
}
