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

// Exact invokevirtual of two shapes, both void:
//
//   aload_0; getfield; ifnull <return>; aload_0; getfield <same>;
//   <parameter loads>; invokevirtual; return
//
//   aload_0; invokevirtual <()V hook>; aload_0; <parameter loads>;
//   invokespecial <shape above>; return
//
// The second shape drops the hook only when the hook that virtual
// dispatch would reach is a single return. A null delegate falls
// through. A non-null delegate is a virtual call of the first shape's
// target, emitted after the method body so the fork does not attach
// the rest of the caller. visitMethodInsn, visitLdcInsn, and
// visitInvokeDynamicInsn test the api field before the null check, so
// they do not match.

unsigned targetFieldOffset(Context* context, GcField* field);

bool inTryBlock(MyThread* t, GcCode* code, unsigned ip);

#if TARGET_BYTES_PER_WORD == 8
bool methodAllowsInlineNew(Context* context);
unsigned takeSideIp(Context* context);

const unsigned DelegateMaxFootprint = 16;

class DelegateSlowPath {
 public:
  DelegateSlowPath* next;
  Compiler::State* edge;
  unsigned slowIp;
  unsigned contIp;
  unsigned footprint;
  unsigned fieldOffset;
  unsigned vtableOffset;
  TraceElement* trace;
  ir::Value* recv;
  ir::Value* arg[DelegateMaxFootprint];
};

namespace {

bool delegateBytesEqual(GcByteArray* bytes, const char* literal)
{
  return bytes
         and bytes->length() == strlen(literal) + 1
         and ::strcmp(reinterpret_cast<const char*>(bytes->body().begin()),
                      literal)
                == 0;
}

bool delegateHolesFree(MyThread* t, Context* context, unsigned origin)
{
  GcCode* code = context->method->code();
  if (code == 0 or origin + 3 >= code->length()) {
    return false;
  }
  // invokevirtual from a generated player, or invokespecial of the
  // same adapter from an override that did its own work first.
  {
    uint8_t op = code->body()[origin];
    if (op != invokevirtual and op != invokespecial) {
      return false;
    }
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

bool delegateHasHandler(MyThread* t, GcCode* code)
{
  if (code == 0) {
    return true;
  }
  GcExceptionHandlerTable* eht = cast<GcExceptionHandlerTable>(
      t, code->exceptionHandlerTable());
  return eht and eht->length() != 0;
}

// True when a parameter is a primitive long or double. An array of
// those is still one reference slot.
bool delegateWidePrimitive(GcByteArray* spec)
{
  if (spec == 0) {
    return true;
  }
  const char* s = reinterpret_cast<const char*>(spec->body().begin());
  if (*s != '(') {
    return true;
  }
  ++s;
  while (*s and *s != ')') {
    if (*s == 'J' or *s == 'D') {
      return true;
    }
    if (*s == '[') {
      while (*s == '[') {
        ++s;
      }
      if (*s == 'L') {
        while (*s and *s != ';') {
          ++s;
        }
      }
      if (*s) {
        ++s;
      }
      continue;
    }
    if (*s == 'L') {
      while (*s and *s != ';') {
        ++s;
      }
      if (*s == ';') {
        ++s;
      }
      continue;
    }
    ++s;
  }
  return false;
}

class DelegateReader {
 public:
  const uint8_t* body;
  unsigned length;
  unsigned ip;

  bool has(unsigned n) const
  {
    return ip + n <= length;
  }

  int op()
  {
    if (not has(1)) {
      return -1;
    }
    return body[ip++];
  }

  int u16()
  {
    if (not has(2)) {
      return -1;
    }
    unsigned hi = body[ip++];
    unsigned lo = body[ip++];
    return static_cast<int>((hi << 8) | lo);
  }
};

bool delegateSpecStep(const char*& s, int& kind)
{
  if (*s == 'B' or *s == 'C' or *s == 'I' or *s == 'S' or *s == 'Z') {
    kind = 1;
    ++s;
    return true;
  }
  if (*s == 'F') {
    kind = 2;
    ++s;
    return true;
  }
  if (*s == 'J' or *s == 'D') {
    return false;
  }
  if (*s == 'L') {
    kind = 3;
    ++s;
    while (*s and *s != ';') {
      ++s;
    }
    if (*s != ';') {
      return false;
    }
    ++s;
    return true;
  }
  if (*s == '[') {
    kind = 3;
    while (*s == '[') {
      ++s;
    }
    if (*s == 'L') {
      ++s;
      while (*s and *s != ';') {
        ++s;
      }
      if (*s != ';') {
        return false;
      }
      ++s;
      return true;
    }
    if (*s == 'B' or *s == 'C' or *s == 'I' or *s == 'S' or *s == 'Z'
        or *s == 'F' or *s == 'J' or *s == 'D') {
      ++s;
      return true;
    }
  }
  return false;
}

bool delegateMatchLoad(DelegateReader& r, int kind, unsigned slot)
{
  unsigned base = kind == 1 ? iload_0 : kind == 2 ? fload_0 : aload_0;
  unsigned wide = kind == 1 ? iload : kind == 2 ? fload : aload;
  if (not r.has(1)) {
    return false;
  }
  unsigned op = r.body[r.ip];
  if (slot <= 3 and op == base + slot) {
    r.ip += 1;
    return true;
  }
  if (op != wide or not r.has(2)) {
    return false;
  }
  if (r.body[r.ip + 1] != slot) {
    return false;
  }
  r.ip += 2;
  return true;
}

bool delegateMatchParams(DelegateReader& r, GcByteArray* spec)
{
  if (spec == 0) {
    return false;
  }
  const char* s = reinterpret_cast<const char*>(spec->body().begin());
  if (*s != '(') {
    return false;
  }
  ++s;
  unsigned slot = 1;
  while (*s != ')') {
    int kind = 0;
    if (not delegateSpecStep(s, kind)) {
      return false;
    }
    if (not delegateMatchLoad(r, kind, slot)) {
      return false;
    }
    ++slot;
  }
  return true;
}

struct DelegateShape {
  unsigned fieldOffset;
  unsigned vtableOffset;
  unsigned footprint;
};

bool delegateMatchNull(MyThread* t,
                       Context* context,
                       GcMethod* method,
                       DelegateShape* shape)
{
  GcCode* code = method->code();
  if (code == 0 or delegateHasHandler(t, code)
      or delegateWidePrimitive(method->spec())) {
    return false;
  }
  if ((method->flags() & (ACC_NATIVE | ACC_ABSTRACT | ACC_SYNCHRONIZED
                          | ACC_STATIC))
      or method->returnCode() != VoidField) {
    return false;
  }
  unsigned footprint = method->parameterFootprint();
  if (footprint < 1 or footprint > DelegateMaxFootprint) {
    return false;
  }

  DelegateReader r;
  r.body = code->body().begin();
  r.length = code->length();
  r.ip = 0;

  if (r.op() != aload_0 or r.op() != getfield) {
    return false;
  }
  int fieldIndex = r.u16();
  unsigned ifAt = r.ip;
  if (fieldIndex <= 0 or r.op() != ifnull) {
    return false;
  }
  int rel = r.u16();
  if (rel < 0) {
    return false;
  }
  rel = static_cast<int16_t>(rel);
  if (rel <= 0) {
    return false;
  }
  unsigned retAt = ifAt + static_cast<unsigned>(rel);
  if (r.op() != aload_0 or r.op() != getfield) {
    return false;
  }
  int fieldIndex2 = r.u16();
  if (fieldIndex2 != fieldIndex) {
    return false;
  }
  if (not delegateMatchParams(r, method->spec())) {
    return false;
  }
  if (r.op() != invokevirtual) {
    return false;
  }
  int methodIndex = r.u16();
  unsigned returnAt = r.ip;
  if (methodIndex <= 0 or r.op() != return_ or r.ip != r.length
      or returnAt != retAt) {
    return false;
  }

  PROTECT(t, method);
  GcField* field = resolveField(
      t, method, static_cast<unsigned>(fieldIndex) - 1, false);
  if (field == 0
      or (field->flags() & (ACC_STATIC | ACC_VOLATILE))
      or field->code() != ObjectField
      or classNeedsInit(t, field->class_())) {
    return false;
  }

  PROTECT(t, field);
  GcMethod* target = resolveMethod(
      t, method, static_cast<unsigned>(methodIndex) - 1, false);
  if (target == 0 or not methodVirtual(t, target)
      or target->returnCode() != VoidField
      or target->parameterFootprint() != footprint) {
    return false;
  }

  shape->fieldOffset = targetFieldOffset(context, field);
  shape->vtableOffset
      = TargetClassVtable + (target->offset() * TargetBytesPerWord);
  shape->footprint = footprint;
  return true;
}

// The hook invokevirtual reaches whatever occupies that vtable slot on
// the exact receiver. A private same-name method is not that slot.
bool delegateHookIsEmpty(MyThread* t, GcMethod* hook, GcClass* recvClass)
{
  if (hook == 0 or not methodVirtual(t, hook)
      or not delegateBytesEqual(hook->spec(), "()V")) {
    return false;
  }
  GcMethod* impl = hook;
  if (recvClass) {
    GcArray* table = cast<GcArray>(t, recvClass->virtualTable());
    if (table == 0 or hook->offset() >= table->length()) {
      return false;
    }
    impl = findVirtualMethod(t, hook, recvClass);
  }
  return impl and impl->code() and emptyMethod(t, impl);
}

bool delegateMatchHook(MyThread* t,
                       Context* context,
                       GcMethod* method,
                       GcClass* recvClass,
                       DelegateShape* shape)
{
  GcCode* code = method->code();
  if (code == 0 or recvClass == 0 or delegateHasHandler(t, code)
      or delegateWidePrimitive(method->spec())) {
    return false;
  }
  if ((method->flags() & (ACC_NATIVE | ACC_ABSTRACT | ACC_SYNCHRONIZED
                          | ACC_STATIC))
      or method->returnCode() != VoidField) {
    return false;
  }

  DelegateReader r;
  r.body = code->body().begin();
  r.length = code->length();
  r.ip = 0;

  if (r.op() != aload_0 or r.op() != invokevirtual) {
    return false;
  }
  int hookIndex = r.u16();
  if (hookIndex <= 0 or r.op() != aload_0) {
    return false;
  }
  if (not delegateMatchParams(r, method->spec())) {
    return false;
  }
  if (r.op() != invokespecial) {
    return false;
  }
  int superIndex = r.u16();
  if (superIndex <= 0 or r.op() != return_ or r.ip != r.length) {
    return false;
  }

  PROTECT(t, method);
  PROTECT(t, recvClass);
  GcMethod* hook = resolveMethod(
      t, method, static_cast<unsigned>(hookIndex) - 1, false);
  PROTECT(t, hook);
  if (not delegateHookIsEmpty(t, hook, recvClass)) {
    return false;
  }

  GcMethod* super = resolveMethod(
      t, method, static_cast<unsigned>(superIndex) - 1, false);
  if (super == 0) {
    return false;
  }
  return delegateMatchNull(t, context, super, shape)
         and shape->footprint == method->parameterFootprint();
}

void queueDelegateSlowPath(Context* context, DelegateSlowPath* path)
{
  path->next = 0;
  if (context->delegateSlowPathTail) {
    context->delegateSlowPathTail->next = path;
  } else {
    context->delegateSlowPaths = path;
  }
  context->delegateSlowPathTail = path;
}

}  // namespace
#endif

bool tryInlineExactDelegate(MyThread* t,
                            Frame* frame,
                            GcMethod* target,
                            GcClass* staticClass)
{
#if TARGET_BYTES_PER_WORD == 8
  Context* context = frame->context;
  if (context->bootContext or frame->subroutine
      or not methodAllowsInlineNew(context)) {
    return false;
  }
  unsigned origin = frame->ip;
  if (inTryBlock(t, context->method->code(), origin)
      or not delegateHolesFree(t, context, origin)) {
    return false;
  }
  if (target == 0 or not methodVirtual(t, target)
      or target->code() == 0
      or target->returnCode() != VoidField
      or (target->flags() & (ACC_NATIVE | ACC_ABSTRACT | ACC_SYNCHRONIZED))) {
    return false;
  }

  bool classExact = staticClass and (staticClass->flags() & ACC_FINAL);
  bool methodExact = (target->flags() & ACC_FINAL) != 0;
  if (not classExact and not methodExact) {
    return false;
  }

  // The hook is a virtual call. It is exact only when no subclass of
  // the receiver exists, or the static type itself is final.
  GcClass* recvClass = 0;
  if (classExact) {
    recvClass = staticClass;
  } else if (target->class_() and (target->class_()->flags() & ACC_FINAL)) {
    recvClass = target->class_();
  }

  PROTECT(t, target);
  PROTECT(t, staticClass);
  PROTECT(t, recvClass);

  DelegateShape shape;
  bool matched = delegateMatchNull(t, context, target, &shape);
  if (not matched and recvClass) {
    matched = delegateMatchHook(t, context, target, recvClass, &shape);
  }
  if (not matched or shape.footprint != target->parameterFootprint()) {
    return false;
  }

  Compiler* c = context->compiler;
  unsigned footprint = shape.footprint;
  ir::Value* recv = c->peek(1, footprint - 1);
  ir::Value* args[DelegateMaxFootprint];
  args[0] = recv;
  for (unsigned i = 1; i < footprint; ++i) {
    args[i] = c->peek(1, footprint - 1 - i);
  }
  TraceElement* trace = frame->trace(0, 0);

  unsigned fastIp = origin + 1;
  unsigned contIp = origin + 2;
  unsigned slowIp = takeSideIp(context);
  ir::Value* slow = c->promiseConstant(c->machineIp(slowIp), ir::Type::iptr());
  ir::Value* delegate = c->load(
      ir::ExtendMode::Signed,
      c->memory(recv, ir::Type::object(), static_cast<int>(shape.fieldOffset)),
      ir::Type::object());
  c->condJump(lir::JumpIfNotEqual,
              c->constant(0, ir::Type::object()),
              delegate,
              slow);
  Compiler::State* edge = c->saveState();
  c->startLogicalIp(fastIp);
  c->startLogicalIp(contIp);
  frame->popFootprint(footprint);

  DelegateSlowPath* path = new (context->zone.allocate(sizeof(DelegateSlowPath)))
      DelegateSlowPath;
  path->edge = edge;
  path->slowIp = slowIp;
  path->contIp = contIp;
  path->footprint = footprint;
  path->fieldOffset = shape.fieldOffset;
  path->vtableOffset = shape.vtableOffset;
  path->trace = trace;
  path->recv = recv;
  for (unsigned i = 0; i < footprint; ++i) {
    path->arg[i] = args[i];
  }
  queueDelegateSlowPath(context, path);
  return true;
#else
  (void)t;
  (void)frame;
  (void)target;
  (void)staticClass;
  return false;
#endif
}

void flushDelegateIntrinsics(MyThread* t, Context* context)
{
#if TARGET_BYTES_PER_WORD == 8
  (void)t;
  Compiler* c = context->compiler;
  for (DelegateSlowPath* path = context->delegateSlowPaths; path;
       path = path->next) {
    c->restoreState(path->edge);
    c->startLogicalIp(path->slowIp);

    ir::Value* delegate = c->load(
        ir::ExtendMode::Signed,
        c->memory(path->recv,
                  ir::Type::object(),
                  static_cast<int>(path->fieldOffset)),
        ir::Type::object());
    ir::Value* callArgs[DelegateMaxFootprint];
    callArgs[0] = delegate;
    for (unsigned i = 1; i < path->footprint; ++i) {
      callArgs[i] = path->arg[i];
    }
    ir::Value* methodValue = c->memory(
        c->binaryOp(lir::And,
                    ir::Type::iptr(),
                    c->constant(TargetPointerMask, ir::Type::iptr()),
                    c->memory(delegate, ir::Type::object())),
        ir::Type::object(),
        static_cast<int>(path->vtableOffset));
    c->stackCall(methodValue,
                 0,
                 path->trace,
                 ir::Type::void_(),
                 Slice<ir::Value*>(callArgs, path->footprint));
    c->jmp(c->promiseConstant(c->machineIp(path->contIp), ir::Type::iptr()));
    c->visitLogicalIp(path->contIp);
  }
  context->delegateSlowPaths = 0;
  context->delegateSlowPathTail = 0;
#else
  (void)t;
  (void)context;
#endif
}
