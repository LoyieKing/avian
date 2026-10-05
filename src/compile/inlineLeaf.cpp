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

// Exact instance getter: aload_0; getfield; areturn/ireturn/freturn.
// No branches, so the caller's control-flow graph is unchanged. The
// receiver is popped and the field is pushed, which matches the call.

unsigned targetFieldOffset(Context* context, GcField* field);

bool inTryBlock(MyThread* t, GcCode* code, unsigned ip);

GcMethod* getterTarget(MyThread* t,
                       GcMethod* target,
                       GcClass* staticClass,
                       bool special)
{
  if (target == 0 or (target->flags() & ACC_STATIC)) {
    return 0;
  }
  if (special or (target->flags() & (ACC_FINAL | ACC_PRIVATE))) {
    return target;
  }
  if (staticClass and (staticClass->flags() & ACC_FINAL)
      and methodVirtual(t, target)) {
    return findVirtualMethod(t, target, staticClass);
  }
  return 0;
}

bool tryInlineGetter(MyThread* t, Frame* frame, GcMethod* target)
{
  if (target == 0 or frame->context->bootContext or debug::enabled()) {
    return false;
  }
  if (target->flags()
      & (ACC_STATIC | ACC_NATIVE | ACC_ABSTRACT | ACC_SYNCHRONIZED)) {
    return false;
  }
  if (target->parameterFootprint() != 1) {
    return false;
  }
  GcCode* code = target->code();
  if (code == 0 or code->length() != 5 or code->exceptionHandlerTable()) {
    return false;
  }
  if (classNeedsInit(t, target->class_())) {
    return false;
  }
  GcCode* caller = frame->context->method->code();
  if (caller == 0 or inTryBlock(t, caller, frame->ip)) {
    return false;
  }

  const uint8_t* body = reinterpret_cast<const uint8_t*>(code->body().begin());
  if (body[0] != aload_0 or body[1] != getfield) {
    return false;
  }
  unsigned ret = body[4];
  uint16_t index
      = static_cast<uint16_t>((static_cast<uint16_t>(body[2]) << 8) | body[3]);
  if (index == 0) {
    return false;
  }

  PROTECT(t, target);
  GcField* field = resolveField(t, target, index - 1, false);
  if (field == 0 or (field->flags() & (ACC_STATIC | ACC_VOLATILE))) {
    return false;
  }

  unsigned kind = field->code();
  ir::Type type = ir::Type::i4();
  ir::ExtendMode extend = ir::ExtendMode::Signed;
  ir::Type mem = ir::Type::i4();
  if (ret == areturn and kind == ObjectField) {
    type = ir::Type::object();
    mem = ir::Type::object();
  } else if (ret == freturn and kind == FloatField) {
    type = ir::Type::f4();
    mem = ir::Type::f4();
  } else if (ret == ireturn
             and (kind == IntField or kind == ByteField or kind == BooleanField
                  or kind == ShortField or kind == CharField)) {
    if (kind == ByteField or kind == BooleanField) {
      mem = ir::Type::i1();
    } else if (kind == ShortField) {
      mem = ir::Type::i2();
    } else if (kind == CharField) {
      mem = ir::Type::i2();
      extend = ir::ExtendMode::Unsigned;
    }
  } else {
    return false;
  }

  Compiler* c = frame->c;
  ir::Value* receiver = frame->pop(ir::Type::object());
  frame->push(type,
              c->load(extend,
                      c->memory(receiver, mem, targetFieldOffset(frame->context, field)),
                      type));
  return true;
}
