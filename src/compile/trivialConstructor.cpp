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

#include "compile/trivialConstructor.h"
#include "compile/objectStore.h"

bool inTryBlock(MyThread* t, GcCode* code, unsigned ip);
bool needsReturnBarrier(MyThread* t, GcMethod* method);

// A constructor body we will inline: aload_0, invokespecial of an empty
// <init>()V, then only (aload_0, one parameter load, putfield), then return.
// Computed values, branches, and other calls stay a real invoke.
enum {
  InlineInitMaxBytes = 128,
  InlineInitMaxStores = 16,
  InlineInitInt = 1,
  InlineInitLong = 2,
  InlineInitFloat = 3,
  InlineInitDouble = 4,
  InlineInitObject = 5
};

struct InlineInitStore {
  uint16_t poolIndex;
  uint8_t local;
  uint8_t slots;
  uint8_t kind;
};

bool inlineInitReadLoad(const uint8_t* body,
                        unsigned length,
                        unsigned* pc,
                        InlineInitStore* store)
{
  if (*pc >= length) {
    return false;
  }

  unsigned op = body[(*pc)++];
  unsigned local;
  unsigned slots;
  unsigned kind;

  if (op == iload or op == lload or op == fload or op == dload or op == aload) {
    if (*pc >= length) {
      return false;
    }
    local = body[(*pc)++];
    if (op == lload or op == dload) {
      slots = 2;
    } else {
      slots = 1;
    }
    if (op == iload) {
      kind = InlineInitInt;
    } else if (op == lload) {
      kind = InlineInitLong;
    } else if (op == fload) {
      kind = InlineInitFloat;
    } else if (op == dload) {
      kind = InlineInitDouble;
    } else {
      kind = InlineInitObject;
    }
  } else if (op >= iload_0 and op <= iload_3) {
    local = op - iload_0;
    slots = 1;
    kind = InlineInitInt;
  } else if (op >= lload_0 and op <= lload_3) {
    local = op - lload_0;
    slots = 2;
    kind = InlineInitLong;
  } else if (op >= fload_0 and op <= fload_3) {
    local = op - fload_0;
    slots = 1;
    kind = InlineInitFloat;
  } else if (op >= dload_0 and op <= dload_3) {
    local = op - dload_0;
    slots = 2;
    kind = InlineInitDouble;
  } else if (op >= aload_0 and op <= aload_3) {
    local = op - aload_0;
    slots = 1;
    kind = InlineInitObject;
  } else {
    return false;
  }

  store->local = static_cast<uint8_t>(local);
  store->slots = static_cast<uint8_t>(slots);
  store->kind = static_cast<uint8_t>(kind);
  return true;
}

bool inlineInitLoadMatches(unsigned kind, unsigned fieldCode)
{
  switch (kind) {
  case InlineInitInt:
    return fieldCode == ByteField or fieldCode == BooleanField
           or fieldCode == CharField or fieldCode == ShortField
           or fieldCode == IntField;

  case InlineInitLong:
    return fieldCode == LongField;

  case InlineInitFloat:
    return fieldCode == FloatField;

  case InlineInitDouble:
    return fieldCode == DoubleField;

  case InlineInitObject:
    return fieldCode == ObjectField;

  default:
    return false;
  }
}

// True when target is a constructor tryCompile will inline, and every
// instance field of class_ and its supers is one of those stores.
// objectStores counts reference stores. lastObject is the index of the
// last of them, or -1. A reference store that is not a plain store is
// a call, and the field would still be uninitialized at that call.
bool trivialInitStoresAllFields(MyThread* t,
                                GcMethod* target,
                                GcClass* class_,
                                unsigned* objectStores,
                                int* lastObject,
                                unsigned* storeCountOut)
{
  if (debug::enabled() or target == 0 or class_ == 0) {
    return false;
  }

  if (target->class_() != class_) {
    return false;
  }

  if (target->flags() & (ACC_NATIVE | ACC_SYNCHRONIZED | ACC_STATIC)) {
    return false;
  }

  if (target->returnCode() != VoidField or target->parameterFootprint() == 0) {
    return false;
  }

  if (strcmp(reinterpret_cast<const int8_t*>("<init>"),
             target->name()->body().begin())
      != 0) {
    return false;
  }

  if (classNeedsInit(t, target->class_())) {
    return false;
  }

  GcCode* code = target->code();
  if (code == 0) {
    return false;
  }

  unsigned length = code->length();
  if (length < 5 or length > InlineInitMaxBytes) {
    return false;
  }

  object handlers = code->exceptionHandlerTable();
  if (handlers) {
    GcExceptionHandlerTable* table
        = cast<GcExceptionHandlerTable>(t, handlers);
    if (table->length()) {
      return false;
    }
  }

  uint8_t body[InlineInitMaxBytes];
  for (unsigned i = 0; i < length; ++i) {
    body[i] = code->body()[i];
  }

  if (body[0] != aload_0 or body[1] != invokespecial) {
    return false;
  }

  unsigned superIndex = (static_cast<unsigned>(body[2]) << 8) | body[3];
  if (superIndex == 0) {
    return false;
  }

  InlineInitStore stores[InlineInitMaxStores];
  unsigned storeCount = 0;
  unsigned pc = 4;
  while (pc < length and body[pc] != return_) {
    if (storeCount == InlineInitMaxStores or body[pc] != aload_0) {
      return false;
    }
    ++pc;

    if (not inlineInitReadLoad(body, length, &pc, stores + storeCount)) {
      return false;
    }

    if (pc >= length or body[pc] != putfield) {
      return false;
    }
    ++pc;
    if (pc + 1 >= length) {
      return false;
    }
    unsigned index = (static_cast<unsigned>(body[pc]) << 8) | body[pc + 1];
    pc += 2;
    if (index == 0) {
      return false;
    }
    stores[storeCount].poolIndex = static_cast<uint16_t>(index);
    ++storeCount;
  }

  if (pc >= length or body[pc] != return_ or pc + 1 != length) {
    return false;
  }

  uint8_t widths[256];
  uint8_t kinds[256];
  memset(widths, 0, sizeof widths);
  memset(kinds, 0, sizeof kinds);
  widths[0] = 1;
  kinds[0] = InlineInitObject;

  const int8_t* spec = target->spec()->body().begin();
  uintptr_t specLength = target->spec()->length();
  if (specLength == 0 or spec[0] != '(') {
    return false;
  }

  unsigned slot = 1;
  unsigned specIndex = 1;
  while (specIndex < specLength and spec[specIndex] != ')') {
    if (slot >= 256) {
      return false;
    }
    unsigned width = 1;
    unsigned kind = InlineInitInt;
    unsigned start = slot;
    int8_t ch = spec[specIndex];
    if (ch == 'J' or ch == 'D') {
      width = 2;
      kind = ch == 'J' ? InlineInitLong : InlineInitDouble;
      ++specIndex;
    } else if (ch == 'F') {
      kind = InlineInitFloat;
      ++specIndex;
    } else if (ch == 'L') {
      kind = InlineInitObject;
      ++specIndex;
      while (specIndex < specLength and spec[specIndex] != ';') {
        ++specIndex;
      }
      if (specIndex >= specLength or spec[specIndex] != ';') {
        return false;
      }
      ++specIndex;
    } else if (ch == '[') {
      kind = InlineInitObject;
      while (specIndex < specLength and spec[specIndex] == '[') {
        ++specIndex;
      }
      if (specIndex >= specLength) {
        return false;
      }
      if (spec[specIndex] == 'L') {
        while (specIndex < specLength and spec[specIndex] != ';') {
          ++specIndex;
        }
      }
      if (specIndex >= specLength) {
        return false;
      }
      ++specIndex;
    } else if (ch == 'B' or ch == 'C' or ch == 'I' or ch == 'S' or ch == 'Z') {
      ++specIndex;
    } else {
      return false;
    }

    if (start + width > 256) {
      return false;
    }
    widths[start] = static_cast<uint8_t>(width);
    kinds[start] = static_cast<uint8_t>(kind);
    slot += width;
  }

  if (specIndex >= specLength or spec[specIndex] != ')'
      or slot != target->parameterFootprint()) {
    return false;
  }

  unsigned footprint = target->parameterFootprint();
  for (unsigned i = 0; i < storeCount; ++i) {
    unsigned local = stores[i].local;
    unsigned slots = stores[i].slots;
    if (local + slots > footprint or widths[local] != slots
        or kinds[local] != stores[i].kind) {
      return false;
    }
  }

  PROTECT(t, target);
  PROTECT(t, class_);

  {
    GcMethod* super = resolveMethod(t, target, superIndex - 1, false);
    if (super == 0) {
      return false;
    }
    PROTECT(t, super);
    if (not emptyMethod(t, super) or super->parameterFootprint() != 1
        or strcmp(reinterpret_cast<const int8_t*>("<init>"),
                  super->name()->body().begin())
               != 0
        or classNeedsInit(t, super->class_())) {
      return false;
    }
  }

  int fieldOffsets[InlineInitMaxStores];
  unsigned fieldCodes[InlineInitMaxStores];
  for (unsigned i = 0; i < storeCount; ++i) {
    GcField* field = resolveField(t, target, stores[i].poolIndex - 1, false);
    if (field == 0) {
      return false;
    }
    PROTECT(t, field);
    if ((field->flags() & (ACC_VOLATILE | ACC_STATIC))
        or not inlineInitLoadMatches(stores[i].kind, field->code())
        or field->class_() != class_) {
      return false;
    }
    fieldOffsets[i] = field->offset();
    fieldCodes[i] = field->code();
  }

  unsigned references = 0;
  int lastReference = -1;
  for (unsigned i = 0; i < storeCount; ++i) {
    if (fieldCodes[i] == ObjectField) {
      ++references;
      lastReference = static_cast<int>(i);
    }
  }

  for (GcClass* type = class_; type; type = type->super()) {
    object table = type->fieldTable();
    if (table == 0) {
      continue;
    }
    for (unsigned i = 0; i < objectArrayLength(t, table); ++i) {
      GcField* field = cast<GcField>(t, objectArrayBody(t, table, i));
      if (field->flags() & ACC_STATIC) {
        continue;
      }
      bool found = false;
      for (unsigned s = 0; s < storeCount; ++s) {
        if (fieldOffsets[s] == field->offset()) {
          found = true;
          break;
        }
      }
      if (not found) {
        return false;
      }
    }
  }

  *objectStores = references;
  *lastObject = lastReference;
  *storeCountOut = storeCount;
  return true;
}

// True when the invoke was replaced by the constructor's stores. False
// leaves the operand stack untouched so the normal call can run.
bool TrivialConstructor::tryCompile(MyThread* t,
                          Frame* frame,
                          GcMethod* target,
                          unsigned callIp)
{
  if (debug::enabled() or frame->context->bootContext or frame->subroutine) {
    return false;
  }

  if (target->flags() & (ACC_NATIVE | ACC_SYNCHRONIZED | ACC_STATIC)) {
    return false;
  }

  if (target->returnCode() != VoidField or target->parameterFootprint() == 0) {
    return false;
  }

  if (strcmp(reinterpret_cast<const int8_t*>("<init>"),
             target->name()->body().begin())
      != 0) {
    return false;
  }

  // A class still needing initialization takes the thunk, including when
  // another thread is the initializer. This thread's own <clinit> does not
  // need init, so a constructor called from it can still be inlined.
  if (classNeedsInit(t, target->class_())) {
    return false;
  }

  GcCode* code = target->code();
  if (code == 0) {
    return false;
  }

  unsigned length = code->length();
  if (length < 5 or length > InlineInitMaxBytes) {
    return false;
  }

  object handlers = code->exceptionHandlerTable();
  if (handlers) {
    GcExceptionHandlerTable* table
        = cast<GcExceptionHandlerTable>(t, handlers);
    if (table->length()) {
      return false;
    }
  }

  // Copy the bytes before any resolve. Those can collect and move the code.
  uint8_t body[InlineInitMaxBytes];
  for (unsigned i = 0; i < length; ++i) {
    body[i] = code->body()[i];
  }

  if (body[0] != aload_0 or body[1] != invokespecial) {
    return false;
  }

  unsigned superIndex
      = (static_cast<unsigned>(body[2]) << 8) | body[3];
  if (superIndex == 0) {
    return false;
  }

  InlineInitStore stores[InlineInitMaxStores];
  unsigned storeCount = 0;
  unsigned pc = 4;
  while (pc < length and body[pc] != return_) {
    if (storeCount == InlineInitMaxStores or body[pc] != aload_0) {
      return false;
    }
    ++pc;

    if (not inlineInitReadLoad(body, length, &pc, stores + storeCount)) {
      return false;
    }

    if (pc >= length or body[pc] != putfield) {
      return false;
    }
    ++pc;
    if (pc + 1 >= length) {
      return false;
    }
    unsigned index
        = (static_cast<unsigned>(body[pc]) << 8) | body[pc + 1];
    pc += 2;
    if (index == 0) {
      return false;
    }
    stores[storeCount].poolIndex = static_cast<uint16_t>(index);
    ++storeCount;
  }

  if (pc >= length or body[pc] != return_ or pc + 1 != length) {
    return false;
  }

  uint8_t widths[256];
  uint8_t kinds[256];
  memset(widths, 0, sizeof widths);
  memset(kinds, 0, sizeof kinds);
  widths[0] = 1;
  kinds[0] = InlineInitObject;

  const int8_t* spec = target->spec()->body().begin();
  uintptr_t specLength = target->spec()->length();
  if (specLength == 0 or spec[0] != '(') {
    return false;
  }

  unsigned slot = 1;
  unsigned specIndex = 1;
  while (specIndex < specLength and spec[specIndex] != ')') {
    if (slot >= 256) {
      return false;
    }
    unsigned width = 1;
    unsigned kind = InlineInitInt;
    unsigned start = slot;
    int8_t ch = spec[specIndex];
    if (ch == 'J' or ch == 'D') {
      width = 2;
      kind = ch == 'J' ? InlineInitLong : InlineInitDouble;
      ++specIndex;
    } else if (ch == 'F') {
      kind = InlineInitFloat;
      ++specIndex;
    } else if (ch == 'L') {
      kind = InlineInitObject;
      ++specIndex;
      while (specIndex < specLength and spec[specIndex] != ';') {
        ++specIndex;
      }
      if (specIndex >= specLength or spec[specIndex] != ';') {
        return false;
      }
      ++specIndex;
    } else if (ch == '[') {
      kind = InlineInitObject;
      while (specIndex < specLength and spec[specIndex] == '[') {
        ++specIndex;
      }
      if (specIndex >= specLength) {
        return false;
      }
      if (spec[specIndex] == 'L') {
        while (specIndex < specLength and spec[specIndex] != ';') {
          ++specIndex;
        }
      }
      if (specIndex >= specLength) {
        return false;
      }
      // ';' of a reference element, or the primitive element character.
      ++specIndex;
    } else if (ch == 'B' or ch == 'C' or ch == 'I' or ch == 'S' or ch == 'Z') {
      ++specIndex;
    } else {
      return false;
    }

    if (start + width > 256) {
      return false;
    }
    widths[start] = static_cast<uint8_t>(width);
    kinds[start] = static_cast<uint8_t>(kind);
    slot += width;
  }

  if (specIndex >= specLength or spec[specIndex] != ')'
      or slot != target->parameterFootprint()) {
    return false;
  }

  unsigned footprint = target->parameterFootprint();
  for (unsigned i = 0; i < storeCount; ++i) {
    unsigned local = stores[i].local;
    unsigned slots = stores[i].slots;
    if (local + slots > footprint or widths[local] != slots
        or kinds[local] != stores[i].kind) {
      return false;
    }
  }

  // Resolving the super method and the fields can collect. Snapshot every
  // integer we need, and emit nothing until the whole body is accepted.
  PROTECT(t, target);

  {
    GcMethod* super = resolveMethod(t, target, superIndex - 1, false);
    if (super == 0) {
      return false;
    }
    PROTECT(t, super);
    if (not emptyMethod(t, super) or super->parameterFootprint() != 1
        or strcmp(reinterpret_cast<const int8_t*>("<init>"),
                  super->name()->body().begin())
               != 0
        or classNeedsInit(t, super->class_())) {
      return false;
    }
  }

  int fieldOffsets[InlineInitMaxStores];
  unsigned fieldCodes[InlineInitMaxStores];
  for (unsigned i = 0; i < storeCount; ++i) {
    GcField* field
        = resolveField(t, target, stores[i].poolIndex - 1, false);
    if (field == 0) {
      return false;
    }
    PROTECT(t, field);
    if ((field->flags() & (ACC_VOLATILE | ACC_STATIC))
        or not inlineInitLoadMatches(stores[i].kind, field->code())) {
      return false;
    }
    fieldOffsets[i] = field->offset();
    fieldCodes[i] = field->code();
  }

  bool barrier = needsReturnBarrier(t, target);

  avian::codegen::Compiler* c = frame->c;
  if (storeCount and inTryBlock(t, frame->context->method->code(), callIp)) {
    // Same as putfield: a faulting store in a handler range needs locals
    // in the frame and a map at this ip.
    c->saveLocals();
    frame->trace(0, 0);
  }

  // Peek while the arguments are still on the stack. saveState and
  // setMaybeNull both need those frame homes. Pop once, after the join.
  // ObjectStore does not pop.
  ir::Value* self = 0;
  ir::Value* values[InlineInitMaxStores];
  if (storeCount) {
    self = c->peek(1, footprint - 1);
    for (unsigned i = 0; i < storeCount; ++i) {
      unsigned depth = footprint - (stores[i].local + stores[i].slots);
      values[i] = c->peek(stores[i].slots, depth);
    }
  }

  // The invokespecial has two spare logical bytes, enough for one
  // branch. Use them only for a single object store that is last:
  // the slow path jumps to the join and would skip a later store.
  int youngStore = -1;
  if (storeCount) {
    unsigned objectStores = 0;
    int lastObject = -1;
    for (unsigned i = 0; i < storeCount; ++i) {
      if (fieldCodes[i] == ObjectField) {
        ++objectStores;
        lastObject = static_cast<int>(i);
      }
    }
    if (objectStores == 1 and lastObject + 1 == static_cast<int>(storeCount)) {
      youngStore = lastObject;
    }
  }

  for (unsigned i = 0; i < storeCount; ++i) {
    ir::Value* value = values[i];
    int offset = fieldOffsets[i];
    switch (fieldCodes[i]) {
    case ByteField:
    case BooleanField:
      c->store(value, c->memory(self, ir::Type::i1(), offset));
      break;

    case CharField:
    case ShortField:
      c->store(value, c->memory(self, ir::Type::i2(), offset));
      break;

    case IntField:
      c->store(value, c->memory(self, ir::Type::i4(), offset));
      break;

    case FloatField:
      c->store(value, c->memory(self, ir::Type::f4(), offset));
      break;

    case LongField:
      c->store(value, c->memory(self, ir::Type::i8(), offset));
      break;

    case DoubleField:
      c->store(value, c->memory(self, ir::Type::f8(), offset));
      break;

    case ObjectField:
      // callIp is the invokespecial. The receiver fact was recorded
      // there, before the call's safepoint. allowBranch only for the
      // one object store that is last: the slow path joins after it.
      ObjectStore::store(t,
                         frame,
                         callIp,
                         self,
                         value,
                         offset,
                         invokespecial,
                         static_cast<int>(i) == youngStore);
      break;

    default:
      abort(t);
    }
  }

  if (barrier) {
    c->nullaryOp(lir::StoreStoreBarrier);
  }

  frame->popFootprint(footprint);
  return true;
}

