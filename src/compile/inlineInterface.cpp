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

// invokeinterface otherwise calls out of the method on every invoke.
// A hit compares the receiver class id to this thread's one-slot cache
// and stack-calls the cached code. A miss, a null receiver, and an
// empty cache take the existing thunk, which records the target.

bool isTailCall(MyThread* t,
                GcCode* code,
                unsigned ip,
                GcMethod* caller,
                GcMethod* callee);

#if TARGET_BYTES_PER_WORD == 8
unsigned takeSideIp(Context* context);

namespace {

const unsigned kClassRuntimeDataIndex = 24;
const unsigned kCellBytes = 16;
const unsigned kCodeDisp = 8;
const unsigned kMaxInterfaceSite = 0x00ffffff;

class InterfaceSite {
 public:
  PoolElement* methodPool;
  TraceElement* lookupTrace;
  TraceElement* callTrace;
  unsigned callIp;
  unsigned slowIp;
  unsigned targetField;
  unsigned cacheField;
  unsigned lengthField;
  unsigned siteField;
  unsigned parameterFootprint;
  unsigned returnCode;
  uint32_t siteIndex;
  bool tail;
  bool slowEmitted;
};

}  // namespace

class InterfaceEdge {
 public:
  InterfaceEdge* next;
  InterfaceSite* site;
  Compiler::State* state;
  unsigned targetIp;
};

static uint32_t nextInterfaceSite = 0;

static unsigned threadField(void* field, MyThread* t)
{
  return static_cast<unsigned>(reinterpret_cast<uint8_t*>(field)
                               - reinterpret_cast<uint8_t*>(t));
}

static PoolElement* interfacePool(Context* context, object target)
{
  for (PoolElement* e = context->objectPool; e; e = e->next) {
    if (e->target == target) {
      return e;
    }
  }
  return 0;
}

static void queueInterface(Context* context,
                           InterfaceSite* site,
                           Compiler::State* state,
                           unsigned targetIp)
{
  InterfaceEdge* edge = new (context->zone.allocate(sizeof(InterfaceEdge)))
      InterfaceEdge;
  edge->next = 0;
  edge->site = site;
  edge->state = state;
  edge->targetIp = targetIp;
  if (context->interfaceEdgeTail) {
    context->interfaceEdgeTail->next = edge;
  } else {
    context->interfaceEdges = edge;
  }
  context->interfaceEdgeTail = edge;
}

static ir::Value* loadThread(Compiler* c, unsigned field)
{
  return c->load(ir::ExtendMode::Signed,
                 c->memory(c->threadRegister(), ir::Type::iptr(), field),
                 ir::Type::iptr());
}

static ir::Value* reloadReceiver(Compiler* c)
{
  return c->load(ir::ExtendMode::Signed,
                 c->memory(c->threadRegister(),
                           ir::Type::object(),
                           TARGET_THREAD_VIRTUALCALLTARGET),
                 ir::Type::object());
}

// -1 when value == 0, else 0.
static ir::Value* zeroMask(Compiler* c, ir::Value* value)
{
  ir::Value* negative = c->binaryOp(
      lir::Subtract, ir::Type::iptr(), value, c->constant(0, ir::Type::iptr()));
  ir::Value* bits = c->binaryOp(lir::Or, ir::Type::iptr(), value, negative);
  ir::Value* nonzero = c->binaryOp(lir::ShiftRight,
                                   ir::Type::iptr(),
                                   c->constant(63, ir::Type::iptr()),
                                   bits);
  return c->binaryOp(lir::Xor,
                     ir::Type::iptr(),
                     c->constant(static_cast<int64_t>(-1), ir::Type::iptr()),
                     nonzero);
}

// Taken edge is the slow thunk. Fall-through must be the next byte of
// this invokeinterface: blocks are laid out in logical-ip order, so a
// side ip would run after the call instead of before it.
static void bail(Compiler* c,
                 Context* context,
                 InterfaceSite* site,
                 lir::TernaryOperation op,
                 ir::Value* a,
                 ir::Value* b,
                 unsigned fallIp)
{
  c->condJump(op,
              a,
              b,
              c->promiseConstant(c->machineIp(site->slowIp), ir::Type::iptr()));
  queueInterface(context, site, c->saveState(), site->slowIp);
  c->startLogicalIp(fallIp);
}

class InlineInterface {
 public:
  static bool tryCompile(MyThread* t,
                         Context* context,
                         Frame* frame,
                         GcMethod* target,
                         unsigned nextIp);
  static void flush(MyThread* t, Context* context);
};

bool InlineInterface::tryCompile(MyThread* t,
                                 Context* context,
                                 Frame* frame,
                                 GcMethod* target,
                                 unsigned nextIp)
{
#ifdef VM_STRESS
  return false;
#endif
  if (context->bootContext or frame->subroutine or debug::enabled()) {
    return false;
  }

  GcCode* code = context->method->code();
  unsigned origin = frame->ip;
  if (code == 0 or nextIp >= code->length() or origin + 5 != nextIp) {
    return false;
  }
  // Five-byte invokeinterface. The four operand bytes are the
  // length check, the id check, the cache load, and the stack call,
  // in that order, so each fall-through is the next machine instruction.
  unsigned lenIp = origin + 1;
  unsigned idIp = origin + 2;
  unsigned hitIp = origin + 3;
  unsigned callIp = origin + 4;
  if (context->visitTable[frame->duplicatedIp(lenIp)] != 0
      or context->visitTable[frame->duplicatedIp(idIp)] != 0
      or context->visitTable[frame->duplicatedIp(hitIp)] != 0
      or context->visitTable[frame->duplicatedIp(callIp)] != 0) {
    return false;
  }
  if (target->parameterFootprint() == 0 or target->class_() == 0) {
    return false;
  }

  uint32_t siteIndex = __atomic_fetch_add(&nextInterfaceSite, 1, __ATOMIC_RELAXED);
  if (siteIndex > kMaxInterfaceSite) {
    return false;
  }

  frame->append(target);
  PoolElement* methodPool = interfacePool(context, target);
  if (methodPool == 0) {
    return false;
  }

  InterfaceSite* site = new (context->zone.allocate(sizeof(InterfaceSite)))
      InterfaceSite;
  site->methodPool = methodPool;
  site->lookupTrace = frame->trace(0, 0);
  site->callTrace = frame->trace(0, 0);
  site->callIp = callIp;
  site->slowIp = takeSideIp(context);
  site->targetField = threadField(&(t->interfaceCallTarget), t);
  site->cacheField = threadField(&(t->interfaceCache), t);
  site->lengthField = threadField(&(t->interfaceCacheLength), t);
  site->siteField = threadField(&(t->interfaceSiteIndex), t);
  site->parameterFootprint = target->parameterFootprint();
  site->returnCode = target->returnCode();
  site->siteIndex = siteIndex;
  site->tail = isTailCall(t, code, nextIp, context->method, target);
  site->slowEmitted = false;

  Compiler* c = context->compiler;
  unsigned footprint = site->parameterFootprint;
  int cellDisp = static_cast<int>(siteIndex) * static_cast<int>(kCellBytes);

  ir::Value* receiver = c->peek(1, footprint - 1);
  c->store(receiver,
           c->memory(c->threadRegister(),
                     ir::Type::object(),
                     TARGET_THREAD_VIRTUALCALLTARGET));
  bail(c,
       context,
       site,
       lir::JumpIfEqual,
       c->constant(0, ir::Type::object()),
       receiver,
       lenIp);

  ir::Value* length = loadThread(c, site->lengthField);
  bail(c,
       context,
       site,
       lir::JumpIfGreaterOrEqual,
       length,
       c->constant(static_cast<intptr_t>(siteIndex), ir::Type::iptr()),
       idIp);

  ir::Value* current = reloadReceiver(c);
  ir::Value* raw = c->load(ir::ExtendMode::Signed,
                           c->memory(current, ir::Type::object(), 0),
                           ir::Type::iptr());
  ir::Value* classPtr = c->binaryOp(
      lir::And,
      ir::Type::iptr(),
      c->constant(TargetPointerMask, ir::Type::iptr()),
      raw);
  ir::Value* classId = c->load(
      ir::ExtendMode::Unsigned,
      c->memory(classPtr, ir::Type::i4(), kClassRuntimeDataIndex),
      ir::Type::iptr());
  ir::Value* base = loadThread(c, site->cacheField);
  ir::Value* cachedId = c->load(
      ir::ExtendMode::Unsigned,
      c->memory(base, ir::Type::i4(), cellDisp),
      ir::Type::iptr());
  ir::Value* diff = c->binaryOp(lir::Xor, ir::Type::iptr(), classId, cachedId);
  ir::Value* empty = zeroMask(c, cachedId);
  ir::Value* differ = c->binaryOp(
      lir::Xor,
      ir::Type::iptr(),
      c->constant(static_cast<int64_t>(-1), ir::Type::iptr()),
      zeroMask(c, diff));
  ir::Value* bad = c->binaryOp(lir::Or, ir::Type::iptr(), empty, differ);
  bail(c,
       context,
       site,
       lir::JumpIfNotEqual,
       c->constant(0, ir::Type::iptr()),
       bad,
       hitIp);

  base = loadThread(c, site->cacheField);
  ir::Value* address = c->load(
      ir::ExtendMode::Signed,
      c->memory(base, ir::Type::iptr(), cellDisp + static_cast<int>(kCodeDisp)),
      ir::Type::iptr());
  c->store(address,
           c->memory(c->threadRegister(), ir::Type::iptr(), site->targetField));

  c->startLogicalIp(callIp);
  context->visitTable[frame->duplicatedIp(lenIp)] = 1;
  context->visitTable[frame->duplicatedIp(idIp)] = 1;
  context->visitTable[frame->duplicatedIp(hitIp)] = 1;
  context->visitTable[frame->duplicatedIp(callIp)] = 1;
  ir::Value* result = c->stackCall(
      loadThread(c, site->targetField),
      site->tail ? Compiler::TailJump : 0,
      site->callTrace,
      operandTypeForFieldCode(t, site->returnCode),
      frame->peekMethodArguments(footprint));
  frame->popFootprint(footprint);
  if (site->returnCode != VoidField) {
    frame->pushReturnValue(site->returnCode, result);
  }
  return true;
}

void InlineInterface::flush(MyThread* t, Context* context)
{
  Compiler* c = context->compiler;
  for (InterfaceEdge* edge = context->interfaceEdges; edge; edge = edge->next) {
    InterfaceSite* site = edge->site;
    c->restoreState(edge->state);
    if (edge->targetIp != site->slowIp) {
      continue;
    }
    if (not site->slowEmitted) {
      c->startLogicalIp(site->slowIp);
      c->store(c->constant(static_cast<intptr_t>(site->siteIndex),
                           ir::Type::iptr()),
               c->memory(c->threadRegister(), ir::Type::iptr(), site->siteField));
      ir::Value* instance = reloadReceiver(c);
      ir::Value* method = c->address(ir::Type::object(), site->methodPool);
      ir::Value* result = c->nativeCall(
          c->constant(getThunk(t, findInterfaceMethodFromInstanceThunk),
                      ir::Type::iptr()),
          0,
          site->lookupTrace,
          ir::Type::iptr(),
          args(c->threadRegister(), method, instance));
      c->store(result,
               c->memory(c->threadRegister(), ir::Type::iptr(), site->targetField));
      c->jmp(c->promiseConstant(c->machineIp(site->callIp), ir::Type::iptr()));
      c->visitLogicalIp(site->callIp);
      site->slowEmitted = true;
    } else {
      c->visitLogicalIp(site->slowIp);
    }
  }
  context->interfaceEdges = 0;
  context->interfaceEdgeTail = 0;
}

#else  // not 64-bit

class InterfaceEdge;

class InlineInterface {
 public:
  static bool tryCompile(MyThread* t,
                         Context* context,
                         Frame* frame,
                         GcMethod* target,
                         unsigned nextIp)
  {
    (void)t;
    (void)context;
    (void)frame;
    (void)target;
    (void)nextIp;
    return false;
  }

  static void flush(MyThread* t, Context* context)
  {
    (void)t;
    (void)context;
  }
};

#endif
