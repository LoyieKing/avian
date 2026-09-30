/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include "compile/objectStoreFacts.h"

#include "avian/debug.h"
#include "avian/machine.h"
#include "avian/zone.h"

#include <string.h>

namespace vm {

namespace {

// Bot is unvisited. Val / Val2 are a primitive or the high half of a
// long or double. Top is an exception-handler local: typed as either a
// reference or a primitive, and never Fresh. Fresh is lost at a
// safepoint (the object may have been promoted); NonNull is not.
const uint8_t FactBot = 0;
const uint8_t FactVal = 1;
const uint8_t FactVal2 = 2;
const uint8_t FactNull = 3;
const uint8_t FactNonNull = 4;
const uint8_t FactFresh = 5;
const uint8_t FactMaybe = 6;
const uint8_t FactTop = 7;

// Tag 0 means the stack word is not a copy of a local. Otherwise the
// tag is the local index plus one, so a zeroed slot is untagged.
const unsigned NoTag = 0;

struct Slot {
  uint8_t fact;
  uint16_t tag;
};

const unsigned EffHas = 1;
const unsigned EffSafepoint = 2;
const unsigned EffFresh = 4;
const unsigned EffNonNull = 8;

struct Effect {
  uint8_t fieldCode;
  uint8_t argSlots;
  uint8_t flags;
};

struct Handler {
  uint16_t start;
  uint16_t end;
  uint16_t ip;
};

const unsigned OsfCap = 1000000;

void* osfAlloc(Zone* zone, size_t bytes)
{
  void* p = zone->allocate(bytes);
  memset(p, 0, bytes);
  return p;
}

unsigned osfReadU16(const uint8_t* code, unsigned ip)
{
  return (unsigned(code[ip]) << 8) | unsigned(code[ip + 1]);
}

int osfReadS16(const uint8_t* code, unsigned ip)
{
  return static_cast<int16_t>(osfReadU16(code, ip));
}

int osfReadS32(const uint8_t* code, unsigned ip)
{
  uint32_t v = (uint32_t(code[ip]) << 24) | (uint32_t(code[ip + 1]) << 16)
               | (uint32_t(code[ip + 2]) << 8) | uint32_t(code[ip + 3]);
  return static_cast<int32_t>(v);
}

bool osfTarget(unsigned ip, int offset, unsigned length, unsigned* target)
{
  int64_t t = static_cast<int64_t>(ip) + offset;
  if (t < 0 or t >= static_cast<int64_t>(length)) {
    return false;
  }
  *target = static_cast<unsigned>(t);
  return true;
}

bool osfIsRef(uint8_t fact)
{
  return fact == FactNull or fact == FactNonNull or fact == FactFresh
         or fact == FactMaybe;
}

uint8_t osfDegradeFact(uint8_t fact)
{
  return fact == FactFresh ? FactNonNull : fact;
}

uint8_t osfMergeFact(uint8_t a, uint8_t b)
{
  if (a == b or b == FactBot) {
    return a;
  }
  if (a == FactBot) {
    return b;
  }
  if (a == FactTop or b == FactTop) {
    return FactTop;
  }
  if (a == FactVal or a == FactVal2 or b == FactVal or b == FactVal2) {
    return FactTop;
  }
  if ((a == FactFresh or a == FactNonNull)
      and (b == FactFresh or b == FactNonNull)) {
    return (a == FactFresh and b == FactFresh) ? FactFresh : FactNonNull;
  }
  if (a == FactNull and b == FactNull) {
    return FactNull;
  }
  return FactMaybe;
}

bool osfSimpleLength(unsigned op, unsigned* len)
{
  switch (op) {
  case nop:
  case aconst_null:
  case iconst_m1:
  case iconst_0:
  case iconst_1:
  case iconst_2:
  case iconst_3:
  case iconst_4:
  case iconst_5:
  case lconst_0:
  case lconst_1:
  case fconst_0:
  case fconst_1:
  case fconst_2:
  case dconst_0:
  case dconst_1:
  case iload_0:
  case iload_1:
  case iload_2:
  case iload_3:
  case lload_0:
  case lload_1:
  case lload_2:
  case lload_3:
  case fload_0:
  case fload_1:
  case fload_2:
  case fload_3:
  case dload_0:
  case dload_1:
  case dload_2:
  case dload_3:
  case aload_0:
  case aload_1:
  case aload_2:
  case aload_3:
  case iaload:
  case laload:
  case faload:
  case daload:
  case aaload:
  case baload:
  case caload:
  case saload:
  case istore_0:
  case istore_1:
  case istore_2:
  case istore_3:
  case lstore_0:
  case lstore_1:
  case lstore_2:
  case lstore_3:
  case fstore_0:
  case fstore_1:
  case fstore_2:
  case fstore_3:
  case dstore_0:
  case dstore_1:
  case dstore_2:
  case dstore_3:
  case astore_0:
  case astore_1:
  case astore_2:
  case astore_3:
  case iastore:
  case lastore:
  case fastore:
  case dastore:
  case aastore:
  case bastore:
  case castore:
  case sastore:
  case pop_:
  case pop2:
  case dup:
  case dup_x1:
  case dup_x2:
  case dup2:
  case dup2_x1:
  case dup2_x2:
  case swap:
  case iadd:
  case ladd:
  case fadd:
  case dadd:
  case isub:
  case lsub:
  case fsub:
  case dsub:
  case imul:
  case lmul:
  case fmul:
  case dmul:
  case idiv:
  case ldiv_:
  case fdiv:
  case ddiv:
  case irem:
  case lrem:
  case frem:
  case vm::drem:
  case ineg:
  case lneg:
  case fneg:
  case dneg:
  case ishl:
  case lshl:
  case ishr:
  case lshr:
  case iushr:
  case lushr:
  case iand:
  case land:
  case ior:
  case lor:
  case ixor:
  case lxor:
  case i2l:
  case i2f:
  case i2d:
  case l2i:
  case l2f:
  case l2d:
  case f2i:
  case f2l:
  case f2d:
  case d2i:
  case d2l:
  case d2f:
  case i2b:
  case i2c:
  case i2s:
  case lcmp:
  case fcmpl:
  case fcmpg:
  case dcmpl:
  case dcmpg:
  case ireturn:
  case lreturn:
  case freturn:
  case dreturn:
  case areturn:
  case return_:
  case arraylength:
  case athrow:
  case monitorenter:
  case monitorexit:
    *len = 1;
    return true;

  case bipush:
  case ldc:
  case iload:
  case lload:
  case fload:
  case dload:
  case aload:
  case istore:
  case lstore:
  case fstore:
  case dstore:
  case astore:
  case newarray:
    *len = 2;
    return true;

  case sipush:
  case ldc_w:
  case ldc2_w:
  case iinc:
  case ifeq:
  case ifne:
  case iflt:
  case ifge:
  case ifgt:
  case ifle:
  case if_icmpeq:
  case if_icmpne:
  case if_icmplt:
  case if_icmpge:
  case if_icmpgt:
  case if_icmple:
  case if_acmpeq:
  case if_acmpne:
  case goto_:
  case getstatic:
  case putstatic:
  case getfield:
  case putfield:
  case invokevirtual:
  case invokespecial:
  case invokestatic:
  case new_:
  case anewarray:
  case checkcast:
  case instanceof:
  case ifnull:
  case ifnonnull:
    *len = 3;
    return true;

  case multianewarray:
    *len = 4;
    return true;

  case invokeinterface:
  case goto_w:
    *len = 5;
    return true;

  default:
    return false;
  }
}

bool osfSwitch(const uint8_t* code,
               unsigned length,
               unsigned ip,
               unsigned* end,
               unsigned* targetCount,
               unsigned* targets)
{
  if (ip >= length) {
    return false;
  }
  unsigned base = (ip + 4) & ~3u;
  if (base < ip or base + 8 > length) {
    return false;
  }
  int def = osfReadS32(code, base);
  unsigned defTarget = 0;
  if (not osfTarget(ip, def, length, &defTarget)) {
    return false;
  }

  if (code[ip] == tableswitch) {
    if (base + 12 > length) {
      return false;
    }
    int low = osfReadS32(code, base + 4);
    int high = osfReadS32(code, base + 8);
    if (high < low) {
      return false;
    }
    uint64_t n = static_cast<uint64_t>(static_cast<int64_t>(high) - low) + 1;
    uint64_t bytes = 12 + n * 4;
    if (n > length or static_cast<uint64_t>(base) + bytes > length) {
      return false;
    }
    *targetCount = static_cast<unsigned>(n) + 1;
    *end = base + static_cast<unsigned>(bytes);
    if (targets) {
      targets[0] = defTarget;
      for (unsigned i = 0; i < static_cast<unsigned>(n); ++i) {
        int off = osfReadS32(code, base + 12 + i * 4);
        if (not osfTarget(ip, off, length, targets + i + 1)) {
          return false;
        }
      }
    }
    return true;
  }

  if (code[ip] == lookupswitch) {
    int npairs = osfReadS32(code, base + 4);
    if (npairs < 0) {
      return false;
    }
    uint64_t bytes = 8 + static_cast<uint64_t>(npairs) * 8;
    if (static_cast<uint64_t>(npairs) > length
        or static_cast<uint64_t>(base) + bytes > length) {
      return false;
    }
    *targetCount = static_cast<unsigned>(npairs) + 1;
    *end = base + static_cast<unsigned>(bytes);
    if (targets) {
      targets[0] = defTarget;
      for (int i = 0; i < npairs; ++i) {
        int off = osfReadS32(code, base + 8 + static_cast<unsigned>(i) * 8 + 4);
        if (not osfTarget(ip, off, length, targets + i + 1)) {
          return false;
        }
      }
    }
    return true;
  }

  return false;
}

bool osfForbidden(unsigned op)
{
  return op == jsr or op == jsr_w or op == ret or op == wide
         or op == breakpoint or op == invokedynamic or op == impdep1
         or op == impdep2;
}

// Field and method widths come from the constant pool without resolving.
// A resolved entry is a GcField or a GcMethodHandle. Anything else is a
// GcReference whose spec names the type. Resolving would initialize classes.

bool osfFieldChar(int ch, unsigned* code)
{
  switch (ch) {
  case 'B':
    *code = ByteField;
    return true;
  case 'C':
    *code = CharField;
    return true;
  case 'D':
    *code = DoubleField;
    return true;
  case 'F':
    *code = FloatField;
    return true;
  case 'I':
    *code = IntField;
    return true;
  case 'J':
    *code = LongField;
    return true;
  case 'S':
    *code = ShortField;
    return true;
  case 'V':
    *code = VoidField;
    return true;
  case 'Z':
    *code = BooleanField;
    return true;
  case 'L':
  case '[':
    *code = ObjectField;
    return true;
  default:
    return false;
  }
}

unsigned osfWords(unsigned code)
{
  if (code == LongField or code == DoubleField) {
    return 2;
  }
  if (code == VoidField) {
    return 0;
  }
  return 1;
}

bool osfParseType(const int8_t* s, unsigned n, unsigned* i, unsigned* code)
{
  if (*i >= n) {
    return false;
  }
  int ch = s[*i];
  if (not osfFieldChar(ch, code)) {
    return false;
  }
  if (ch == 'L') {
    while (*i < n and s[*i] != ';') {
      ++(*i);
    }
    if (*i >= n) {
      return false;
    }
    ++(*i);
    return true;
  }
  if (ch == '[') {
    while (*i < n and s[*i] == '[') {
      ++(*i);
    }
    if (*i >= n) {
      return false;
    }
    unsigned ignore = 0;
    return osfParseType(s, n, i, &ignore);
  }
  ++(*i);
  return true;
}

bool osfParseMethod(const int8_t* s,
                    unsigned n,
                    bool instance,
                    unsigned* slots,
                    unsigned* returnCode)
{
  if (n == 0 or s[0] != '(') {
    return false;
  }
  unsigned i = 1;
  unsigned fp = instance ? 1 : 0;
  while (i < n and s[i] != ')') {
    unsigned code = 0;
    if (not osfParseType(s, n, &i, &code)) {
      return false;
    }
    fp += osfWords(code);
    if (fp > 255) {
      return false;
    }
  }
  if (i >= n or s[i] != ')') {
    return false;
  }
  ++i;
  unsigned rc = 0;
  if (not osfParseType(s, n, &i, &rc)) {
    return false;
  }
  *slots = fp;
  *returnCode = rc;
  return true;
}

bool osfMethodEffect(Thread* t, object o, bool instance, Effect* effect)
{
  unsigned slots = 0;
  unsigned rc = 0;
  if (objectClass(t, o) == type(t, GcMethodHandle::Type)) {
    GcMethod* method = cast<GcMethodHandle>(t, o)->method();
    if (method == 0) {
      return false;
    }
    slots = method->parameterFootprint();
    rc = method->returnCode();
  } else if (objectClass(t, o) == type(t, GcReference::Type)) {
    GcReference* reference = cast<GcReference>(t, o);
    GcByteArray* spec = reference->spec();
    if (spec == 0
        or not osfParseMethod(spec->body().begin(),
                              spec->length(),
                              instance,
                              &slots,
                              &rc)) {
      return false;
    }
  } else {
    return false;
  }
  // argSlots is one byte. A wider footprint would pop the wrong words
  // and could record Fresh for a value that is not the receiver.
  if (slots > 255 or rc > 255) {
    return false;
  }
  effect->fieldCode = static_cast<uint8_t>(rc);
  effect->argSlots = static_cast<uint8_t>(slots);
  effect->flags = EffHas | EffSafepoint;
  return true;
}

bool osfFieldEffect(Thread* t, object o, unsigned op, Effect* effect)
{
  unsigned code = 0;
  bool safepoint = false;
  if (objectClass(t, o) == type(t, GcField::Type)) {
    GcField* field = cast<GcField>(t, o);
    code = field->code();
    if ((field->flags() & ACC_VOLATILE) and TargetBytesPerWord == 4
        and (code == LongField or code == DoubleField)) {
      safepoint = true;
    }
    if ((op == getstatic or op == putstatic)
        and classNeedsInit(t, field->class_())) {
      safepoint = true;
    }
  } else if (objectClass(t, o) == type(t, GcReference::Type)) {
    GcReference* reference = cast<GcReference>(t, o);
    GcByteArray* spec = reference->spec();
    if (spec == 0 or spec->length() == 0
        or not osfFieldChar(spec->body()[0], &code)) {
      return false;
    }
    safepoint = true;
  } else {
    return false;
  }
  effect->fieldCode = static_cast<uint8_t>(code);
  effect->argSlots = 0;
  effect->flags = EffHas | (safepoint ? EffSafepoint : 0);
  return true;
}

bool osfNewEffect(Thread* t, object o, Effect* effect)
{
  unsigned flags = EffHas | EffSafepoint | EffNonNull;
  if (o and objectClass(t, o) == type(t, GcClass::Type)) {
    GcClass* class_ = cast<GcClass>(t, o);
    if ((class_->vmFlags() & (WeakReferenceFlag | HasFinalizerFlag)) == 0) {
      flags = EffHas | EffSafepoint | EffFresh;
    }
  }
  effect->fieldCode = ObjectField;
  effect->argSlots = 0;
  effect->flags = static_cast<uint8_t>(flags);
  return true;
}

bool osfLdcEffect(Thread* t, GcSingleton* pool, unsigned index, Effect* effect)
{
  if (index >= poolSize(t, pool)) {
    return false;
  }
  if (not singletonIsObject(t, pool, index)) {
    effect->fieldCode = IntField;
    effect->argSlots = 0;
    effect->flags = EffHas;
    return true;
  }
  object v = singletonObject(t, pool, index);
  unsigned flags = EffHas | EffNonNull;
  if (v == 0) {
    flags = EffHas;
  } else if (objectClass(t, v) == type(t, GcClass::Type)
             or objectClass(t, v) == type(t, GcReference::Type)) {
    // Class constants call getJClass, which can allocate.
    flags = EffHas | EffSafepoint | EffNonNull;
  }
  effect->fieldCode = ObjectField;
  effect->argSlots = 0;
  effect->flags = static_cast<uint8_t>(flags);
  return true;
}

struct FactMachine {
  Slot* local;
  Slot* stack;
  unsigned maxLocals;
  unsigned maxStack;
  unsigned sp;

  Slot* at(unsigned index)
  {
    return stack + index;
  }

  bool push(uint8_t fact, uint16_t tag)
  {
    if (sp >= maxStack) {
      return false;
    }
    stack[sp].fact = fact;
    stack[sp].tag = tag;
    ++sp;
    return true;
  }

  bool pushCat2()
  {
    return push(FactVal, NoTag) and push(FactVal2, NoTag);
  }

  bool pop1(Slot* out)
  {
    if (sp == 0 or stack[sp - 1].fact == FactVal2) {
      return false;
    }
    --sp;
    *out = stack[sp];
    return true;
  }

  bool popInt()
  {
    Slot s;
    if (not pop1(&s)) {
      return false;
    }
    return s.fact == FactVal or s.fact == FactTop;
  }

  bool popRef(Slot* out)
  {
    if (not pop1(out)) {
      return false;
    }
    if (out->fact == FactTop) {
      out->fact = FactMaybe;
      out->tag = NoTag;
      return true;
    }
    return osfIsRef(out->fact);
  }

  bool popCat2()
  {
    if (sp < 2 or stack[sp - 1].fact != FactVal2) {
      return false;
    }
    uint8_t low = stack[sp - 2].fact;
    if (low != FactVal and low != FactTop) {
      return false;
    }
    sp -= 2;
    return true;
  }

  bool popSlots(unsigned n)
  {
    if (sp < n) {
      return false;
    }
    if (n and sp - n < sp and stack[sp - n].fact == FactVal2) {
      return false;
    }
    sp -= n;
    return true;
  }

  void clearTag(unsigned local)
  {
    uint16_t tag = static_cast<uint16_t>(local + 1);
    for (unsigned i = 0; i < sp; ++i) {
      if (stack[i].tag == tag) {
        stack[i].tag = NoTag;
      }
    }
  }

  bool setLocal(unsigned index, uint8_t fact)
  {
    if (index >= maxLocals) {
      return false;
    }
    clearTag(index);
    local[index].fact = fact;
    local[index].tag = NoTag;
    return true;
  }

  bool loadRef(unsigned local, uint8_t* fact, uint16_t* tag)
  {
    if (local >= maxLocals) {
      return false;
    }
    uint8_t f = this->local[local].fact;
    if (f == FactBot) {
      return false;
    }
    if (f == FactTop) {
      *fact = FactMaybe;
      *tag = NoTag;
      return true;
    }
    if (not osfIsRef(f)) {
      return false;
    }
    *fact = f;
    *tag = static_cast<uint16_t>(local + 1);
    return true;
  }

  bool loadInt(unsigned local)
  {
    if (local >= maxLocals) {
      return false;
    }
    uint8_t f = this->local[local].fact;
    if (f != FactVal and f != FactTop) {
      return false;
    }
    return push(FactVal, NoTag);
  }

  bool loadCat2(unsigned local)
  {
    if (local + 1 >= maxLocals) {
      return false;
    }
    uint8_t low = this->local[local].fact;
    uint8_t high = this->local[local + 1].fact;
    if (low == FactTop and high == FactTop) {
      return pushCat2();
    }
    if (low == FactVal and high == FactVal2) {
      return pushCat2();
    }
    return false;
  }

  void degrade()
  {
    for (unsigned i = 0; i < maxLocals; ++i) {
      local[i].fact = osfDegradeFact(local[i].fact);
    }
    for (unsigned i = 0; i < sp; ++i) {
      stack[i].fact = osfDegradeFact(stack[i].fact);
    }
  }
};

struct Pass {
  const uint8_t* bytes;
  unsigned length;
  unsigned maxLocals;
  unsigned maxStack;
  unsigned stride;
  Slot* states;
  unsigned* spOf;
  uint8_t* reach;
  uint8_t* inQ;
  uint8_t* isStart;
  uint16_t* queue;
  unsigned qh;
  unsigned qt;
  unsigned qCount;
  uint8_t* out;
  uint8_t* seen;
  Effect* effects;
  unsigned* switches;
  bool debug;
  unsigned steps;
};

Slot* passSlots(Pass* p, unsigned ip)
{
  return p->states + ip * p->stride;
}

bool passEnqueue(Pass* p, unsigned ip)
{
  if (ip >= p->length or not p->isStart[ip] or p->inQ[ip]) {
    return ip < p->length and p->isStart[ip];
  }
  if (p->qCount >= p->length) {
    return false;
  }
  p->inQ[ip] = 1;
  p->queue[p->qt] = static_cast<uint16_t>(ip);
  p->qt = (p->qt + 1) % p->length;
  ++p->qCount;
  return true;
}

void passCopy(Pass* p, unsigned ip, FactMachine* m, Slot* locals, Slot* stack)
{
  m->local = locals;
  m->stack = stack;
  m->maxLocals = p->maxLocals;
  m->maxStack = p->maxStack;
  m->sp = p->spOf[ip];
  if (p->stride) {
    memcpy(locals, passSlots(p, ip), p->maxLocals * sizeof(Slot));
    if (m->sp) {
      memcpy(stack,
             passSlots(p, ip) + p->maxLocals,
             m->sp * sizeof(Slot));
    }
  }
}

bool passMerge(Pass* p, unsigned ip, FactMachine* m)
{
  if (ip >= p->length or not p->isStart[ip]) {
    return false;
  }
  if (not p->reach[ip]) {
    p->reach[ip] = 1;
    p->spOf[ip] = m->sp;
    if (p->stride) {
      memcpy(passSlots(p, ip), m->local, p->maxLocals * sizeof(Slot));
      if (m->sp) {
        memcpy(passSlots(p, ip) + p->maxLocals,
               m->stack,
               m->sp * sizeof(Slot));
      }
    }
    return passEnqueue(p, ip);
  }
  if (p->spOf[ip] != m->sp) {
    return false;
  }
  bool changed = false;
  Slot* cur = passSlots(p, ip);
  for (unsigned i = 0; i < p->maxLocals; ++i) {
    uint8_t merged = osfMergeFact(cur[i].fact, m->local[i].fact);
    uint16_t tag = (cur[i].tag and cur[i].tag == m->local[i].tag) ? cur[i].tag
                                                                  : NoTag;
    if (merged != cur[i].fact or tag != cur[i].tag) {
      cur[i].fact = merged;
      cur[i].tag = tag;
      changed = true;
    }
  }
  Slot* st = cur + p->maxLocals;
  for (unsigned i = 0; i < m->sp; ++i) {
    uint8_t merged = osfMergeFact(st[i].fact, m->stack[i].fact);
    uint16_t tag = (st[i].tag and st[i].tag == m->stack[i].tag) ? st[i].tag
                                                                : NoTag;
    if (merged != st[i].fact or tag != st[i].tag) {
      st[i].fact = merged;
      st[i].tag = tag;
      changed = true;
    }
  }
  if (changed) {
    return passEnqueue(p, ip);
  }
  return true;
}

void passRefine(FactMachine* m, int local, bool asNull)
{
  if (local < 0 or static_cast<unsigned>(local) >= m->maxLocals) {
    return;
  }
  uint8_t f = m->local[local].fact;
  if (not osfIsRef(f)) {
    return;
  }
  if (asNull) {
    m->local[local].fact = FactNull;
  } else if (f != FactFresh) {
    m->local[local].fact = FactNonNull;
  }
}

uint8_t passFactBit(uint8_t fact)
{
  if (fact == FactFresh) {
    return ObjectStoreFacts::Fresh;
  }
  if (fact == FactNonNull) {
    return ObjectStoreFacts::NonNull;
  }
  return 0;
}

// Fresh meeting NonNull is NonNull. Anything meeting "no fact" is no
// fact: a handler or a second path must not leave an earlier Fresh bit
// in place. Unreached ips stay zero because passRecord is not called.
uint8_t passMeetBit(uint8_t a, uint8_t b)
{
  if (a == b) {
    return a;
  }
  if (a == 0 or b == 0) {
    return 0;
  }
  return ObjectStoreFacts::NonNull;
}

void passRecord(Pass* p, unsigned ip, uint8_t fact)
{
  uint8_t bit = passFactBit(fact);
  if (not p->seen[ip]) {
    p->seen[ip] = 1;
    p->out[ip] = bit;
    return;
  }
  p->out[ip] = passMeetBit(p->out[ip], bit);
}

bool passDupX1(FactMachine* m)
{
  Slot v1;
  Slot v2;
  if (not m->pop1(&v1) or not m->pop1(&v2)) {
    return false;
  }
  return m->push(v1.fact, v1.tag) and m->push(v2.fact, v2.tag)
         and m->push(v1.fact, v1.tag);
}

bool passDupX2(FactMachine* m)
{
  if (m->sp >= 1 and m->stack[m->sp - 1].fact != FactVal2 and m->sp >= 2
      and m->stack[m->sp - 2].fact == FactVal2) {
    Slot v1;
    if (not m->pop1(&v1) or not m->popCat2()) {
      return false;
    }
    return m->push(v1.fact, v1.tag) and m->pushCat2()
           and m->push(v1.fact, v1.tag);
  }
  Slot v1;
  Slot v2;
  Slot v3;
  if (not m->pop1(&v1) or not m->pop1(&v2) or not m->pop1(&v3)) {
    return false;
  }
  return m->push(v1.fact, v1.tag) and m->push(v3.fact, v3.tag)
         and m->push(v2.fact, v2.tag) and m->push(v1.fact, v1.tag);
}

bool passDup2(FactMachine* m)
{
  if (m->sp >= 1 and m->stack[m->sp - 1].fact == FactVal2) {
    if (not m->popCat2()) {
      return false;
    }
    return m->pushCat2() and m->pushCat2();
  }
  Slot v1;
  Slot v2;
  if (not m->pop1(&v1) or not m->pop1(&v2)) {
    return false;
  }
  return m->push(v2.fact, v2.tag) and m->push(v1.fact, v1.tag)
         and m->push(v2.fact, v2.tag) and m->push(v1.fact, v1.tag);
}

bool passDup2X1(FactMachine* m)
{
  if (m->sp >= 1 and m->stack[m->sp - 1].fact == FactVal2) {
    if (not m->popCat2()) {
      return false;
    }
    Slot v2;
    if (not m->pop1(&v2)) {
      return false;
    }
    return m->pushCat2() and m->push(v2.fact, v2.tag) and m->pushCat2();
  }
  Slot v1;
  Slot v2;
  Slot v3;
  if (not m->pop1(&v1) or not m->pop1(&v2) or not m->pop1(&v3)) {
    return false;
  }
  return m->push(v2.fact, v2.tag) and m->push(v1.fact, v1.tag)
         and m->push(v3.fact, v3.tag) and m->push(v2.fact, v2.tag)
         and m->push(v1.fact, v1.tag);
}

// The four JVM forms. A category-2 value is detected by its high word
// (FactVal2): form 4 is two of those, form 2 is one on top of two
// category-1 values, form 3 is two category-1 values on top of one,
// and form 1 is four category-1 values.
bool passDup2X2(FactMachine* m)
{
  if (m->sp >= 1 and m->stack[m->sp - 1].fact == FactVal2) {
    if (m->sp >= 4 and m->stack[m->sp - 3].fact == FactVal2) {
      if (not m->popCat2() or not m->popCat2()) {
        return false;
      }
      return m->pushCat2() and m->pushCat2() and m->pushCat2();
    }
    if (not m->popCat2()) {
      return false;
    }
    Slot a;
    Slot b;
    if (not m->pop1(&a) or not m->pop1(&b)) {
      return false;
    }
    return m->pushCat2() and m->push(b.fact, b.tag) and m->push(a.fact, a.tag)
           and m->pushCat2();
  }
  if (m->sp >= 4 and m->stack[m->sp - 3].fact == FactVal2) {
    Slot v1;
    Slot v2;
    if (not m->pop1(&v1) or not m->pop1(&v2) or not m->popCat2()) {
      return false;
    }
    return m->push(v2.fact, v2.tag) and m->push(v1.fact, v1.tag)
           and m->pushCat2() and m->push(v2.fact, v2.tag)
           and m->push(v1.fact, v1.tag);
  }
  Slot v1;
  Slot v2;
  Slot v3;
  Slot v4;
  if (not m->pop1(&v1) or not m->pop1(&v2) or not m->pop1(&v3)
      or not m->pop1(&v4)) {
    return false;
  }
  return m->push(v2.fact, v2.tag) and m->push(v1.fact, v1.tag)
         and m->push(v4.fact, v4.tag) and m->push(v3.fact, v3.tag)
         and m->push(v2.fact, v2.tag) and m->push(v1.fact, v1.tag);
}

int passNullTag(FactMachine* m, const Slot& a, const Slot& b, bool* nullOnTrue)
{
  if (a.fact == FactNull and b.tag) {
    *nullOnTrue = true;
    return static_cast<int>(b.tag - 1);
  }
  if (b.fact == FactNull and a.tag) {
    *nullOnTrue = true;
    return static_cast<int>(a.tag - 1);
  }
  (void)m;
  return -1;
}

bool passTransfer(Pass* p, unsigned ip, FactMachine* m, Slot* edgeLocal, Slot* edgeStack)
{
  if (p->debug) {
    m->degrade();
  }
  unsigned op = p->bytes[ip];
  unsigned next = 0;
  unsigned len = 0;
  if (op == tableswitch or op == lookupswitch) {
    unsigned end = 0;
    unsigned count = 0;
    if (not osfSwitch(p->bytes, p->length, ip, &end, &count, 0)
        or count > p->length) {
      return false;
    }
    if (not osfSwitch(p->bytes, p->length, ip, &end, &count, p->switches)) {
      return false;
    }
    if (not m->popInt()) {
      return false;
    }
    // lookupswitch calls lookUpAddress when it has pairs. That call does
    // not allocate. tableswitch is an inline jump. Neither promotes.
    for (unsigned s = 0; s < count; ++s) {
      if (not passMerge(p, p->switches[s], m)) {
        return false;
      }
    }
    (void)next;
    return true;
  }

  if (not osfSimpleLength(op, &len) or ip + len > p->length) {
    return false;
  }
  next = ip + len;
  Effect effect = p->effects[ip];

  switch (op) {
  case nop:
    break;

  case aconst_null:
    if (not m->push(FactNull, NoTag)) {
      return false;
    }
    break;

  case iconst_m1:
  case iconst_0:
  case iconst_1:
  case iconst_2:
  case iconst_3:
  case iconst_4:
  case iconst_5:
  case bipush:
  case sipush:
  case iinc: {
    if (op == iinc) {
      unsigned local = p->bytes[ip + 1];
      if (not m->setLocal(local, FactVal)) {
        return false;
      }
    } else if (not m->push(FactVal, NoTag)) {
      return false;
    }
    break;
  }

  case lconst_0:
  case lconst_1:
  case dconst_0:
  case dconst_1:
  case ldc2_w:
    if (not m->pushCat2()) {
      return false;
    }
    break;

  case fconst_0:
  case fconst_1:
  case fconst_2:
    if (not m->push(FactVal, NoTag)) {
      return false;
    }
    break;

  case ldc:
  case ldc_w: {
    if ((effect.flags & EffHas) == 0) {
      return false;
    }
    if (effect.flags & EffSafepoint) {
      m->degrade();
    }
    if (effect.fieldCode == ObjectField) {
      uint8_t fact = (effect.flags & EffNonNull) ? FactNonNull : FactMaybe;
      if (not m->push(fact, NoTag)) {
        return false;
      }
    } else if (not m->push(FactVal, NoTag)) {
      return false;
    }
    break;
  }

  case iload:
  case fload:
    if (not m->loadInt(p->bytes[ip + 1])) {
      return false;
    }
    break;
  case iload_0:
  case iload_1:
  case iload_2:
  case iload_3:
  case fload_0:
  case fload_1:
  case fload_2:
  case fload_3:
    if (not m->loadInt(op - (op >= fload_0 ? fload_0 : iload_0))) {
      return false;
    }
    break;

  case lload:
  case dload:
    if (not m->loadCat2(p->bytes[ip + 1])) {
      return false;
    }
    break;
  case lload_0:
  case lload_1:
  case lload_2:
  case lload_3:
    if (not m->loadCat2(op - lload_0)) {
      return false;
    }
    break;
  case dload_0:
  case dload_1:
  case dload_2:
  case dload_3:
    if (not m->loadCat2(op - dload_0)) {
      return false;
    }
    break;

  case aload:
  case aload_0:
  case aload_1:
  case aload_2:
  case aload_3: {
    unsigned local = op == aload ? p->bytes[ip + 1] : op - aload_0;
    uint8_t fact = 0;
    uint16_t tag = 0;
    if (not m->loadRef(local, &fact, &tag) or not m->push(fact, tag)) {
      return false;
    }
    break;
  }

  case istore:
  case fstore:
    if (not m->popInt() or not m->setLocal(p->bytes[ip + 1], FactVal)) {
      return false;
    }
    break;
  case lstore:
  case dstore:
    if (not m->popCat2() or not m->setLocal(p->bytes[ip + 1], FactVal)
        or not m->setLocal(p->bytes[ip + 1] + 1, FactVal2)) {
      return false;
    }
    break;

  case istore_0:
  case istore_1:
  case istore_2:
  case istore_3:
  case fstore_0:
  case fstore_1:
  case fstore_2:
  case fstore_3: {
    unsigned local = (op >= fstore_0 and op <= fstore_3) ? op - fstore_0
                                                         : op - istore_0;
    if (not m->popInt() or not m->setLocal(local, FactVal)) {
      return false;
    }
    break;
  }

  case lstore_0:
  case lstore_1:
  case lstore_2:
  case lstore_3:
  case dstore_0:
  case dstore_1:
  case dstore_2:
  case dstore_3: {
    unsigned local = op >= dstore_0 ? op - dstore_0 : op - lstore_0;
    if (not m->popCat2() or not m->setLocal(local, FactVal)
        or not m->setLocal(local + 1, FactVal2)) {
      return false;
    }
    break;
  }

  case astore:
  case astore_0:
  case astore_1:
  case astore_2:
  case astore_3: {
    unsigned local = op == astore ? p->bytes[ip + 1] : op - astore_0;
    Slot s;
    if (not m->popRef(&s) or not m->setLocal(local, s.fact)) {
      return false;
    }
    break;
  }

  case iaload:
  case baload:
  case caload:
  case saload:
  case faload: {
    Slot array;
    if (not m->popInt() or not m->popRef(&array) or not m->push(FactVal, NoTag)) {
      return false;
    }
    break;
  }

  case laload:
  case daload: {
    Slot array;
    if (not m->popInt() or not m->popRef(&array) or not m->pushCat2()) {
      return false;
    }
    break;
  }

  case aaload: {
    Slot array;
    if (not m->popInt() or not m->popRef(&array)
        or not m->push(FactMaybe, NoTag)) {
      return false;
    }
    break;
  }

  case iastore:
  case bastore:
  case castore:
  case sastore:
  case fastore: {
    Slot array;
    if (not m->popInt() or not m->popInt() or not m->popRef(&array)) {
      return false;
    }
    break;
  }

  case lastore:
  case dastore: {
    Slot array;
    if (not m->popCat2() or not m->popInt() or not m->popRef(&array)) {
      return false;
    }
    break;
  }

  case aastore: {
    if (m->sp < 3) {
      return false;
    }
    passRecord(p, ip, m->stack[m->sp - 3].fact);
    Slot value;
    Slot array;
    if (not m->popRef(&value) or not m->popInt() or not m->popRef(&array)) {
      return false;
    }
    break;
  }

  case pop_: {
    Slot s;
    if (not m->pop1(&s)) {
      return false;
    }
    break;
  }

  case pop2:
    if (m->sp >= 1 and m->stack[m->sp - 1].fact == FactVal2) {
      if (not m->popCat2()) {
        return false;
      }
    } else {
      Slot a;
      Slot b;
      if (not m->pop1(&a) or not m->pop1(&b)) {
        return false;
      }
    }
    break;

  case dup: {
    Slot s;
    if (not m->pop1(&s) or not m->push(s.fact, s.tag)
        or not m->push(s.fact, s.tag)) {
      return false;
    }
    break;
  }

  case dup_x1:
    if (not passDupX1(m)) {
      return false;
    }
    break;

  case dup_x2:
    if (not passDupX2(m)) {
      return false;
    }
    break;

  case dup2:
    if (not passDup2(m)) {
      return false;
    }
    break;

  case dup2_x1:
    if (not passDup2X1(m)) {
      return false;
    }
    break;

  case dup2_x2:
    if (not passDup2X2(m)) {
      return false;
    }
    break;

  case swap: {
    Slot a;
    Slot b;
    if (not m->pop1(&a) or not m->pop1(&b) or not m->push(a.fact, a.tag)
        or not m->push(b.fact, b.tag)) {
      return false;
    }
    break;
  }

  case iadd:
  case isub:
  case imul:
  case idiv:
  case irem:
  case ishl:
  case ishr:
  case iushr:
  case iand:
  case ior:
  case ixor:
  case fadd:
  case fsub:
  case fmul:
  case fdiv:
  case frem:
    if (not m->popInt() or not m->popInt() or not m->push(FactVal, NoTag)) {
      return false;
    }
    break;

  case ladd:
  case lsub:
  case lmul:
  case ldiv_:
  case lrem:
  case land:
  case lor:
  case lxor:
  case dadd:
  case dsub:
  case dmul:
  case ddiv:
  case vm::drem:
    if (not m->popCat2() or not m->popCat2() or not m->pushCat2()) {
      return false;
    }
    break;

  case lshl:
  case lshr:
  case lushr:
    if (not m->popInt() or not m->popCat2() or not m->pushCat2()) {
      return false;
    }
    break;

  case ineg:
  case fneg:
    if (not m->popInt() or not m->push(FactVal, NoTag)) {
      return false;
    }
    break;

  case lneg:
  case dneg:
    if (not m->popCat2() or not m->pushCat2()) {
      return false;
    }
    break;

  case i2l:
  case i2d:
    if (not m->popInt() or not m->pushCat2()) {
      return false;
    }
    break;

  case i2f:
  case i2b:
  case i2c:
  case i2s:
  case f2i:
    if (not m->popInt() or not m->push(FactVal, NoTag)) {
      return false;
    }
    break;

  case l2i:
  case l2f:
  case d2i:
  case d2f:
    if (not m->popCat2() or not m->push(FactVal, NoTag)) {
      return false;
    }
    break;

  case l2d:
  case d2l:
    if (not m->popCat2() or not m->pushCat2()) {
      return false;
    }
    break;

  case f2l:
  case f2d:
    if (not m->popInt() or not m->pushCat2()) {
      return false;
    }
    break;

  case lcmp:
  case fcmpl:
  case fcmpg:
  case dcmpl:
  case dcmpg:
    // The compiler emits a call for the unfolded compare. The call does
    // not allocate on success; a failure throws. Leave Fresh in place.
    if (op == lcmp or op == dcmpl or op == dcmpg) {
      if (not m->popCat2() or not m->popCat2()) {
        return false;
      }
    } else if (not m->popInt() or not m->popInt()) {
      return false;
    }
    if (not m->push(FactVal, NoTag)) {
      return false;
    }
    break;

  case arraylength: {
    Slot array;
    if (not m->popRef(&array) or not m->push(FactVal, NoTag)) {
      return false;
    }
    break;
  }

  case getfield:
  case getstatic:
  case putfield:
  case putstatic: {
    if ((effect.flags & EffHas) == 0) {
      return false;
    }
    unsigned words = osfWords(effect.fieldCode);
    bool objectField = effect.fieldCode == ObjectField;
    if (op == putfield and objectField) {
      if (m->sp < words + 1) {
        return false;
      }
      // A pool entry that is still a reference resolves in the helper,
      // and compile() may later emit either that helper or a direct
      // store. NonNull here would drop the helper. Zero keeps
      // setMaybeNull. A resolved put has no safepoint before the write,
      // so the receiver fact is recorded as it stands.
      uint8_t fact = FactMaybe;
      if ((effect.flags & EffSafepoint) == 0) {
        fact = m->stack[m->sp - words - 1].fact;
      }
      passRecord(p, ip, fact);
    }
    if (effect.flags & EffSafepoint) {
      m->degrade();
    }
    if (op == putfield or op == getfield) {
      Slot obj;
      if (op == putfield) {
        if (not m->popSlots(words) or not m->popRef(&obj)) {
          return false;
        }
      } else if (not m->popRef(&obj)) {
        return false;
      }
    } else if (op == putstatic) {
      if (not m->popSlots(words)) {
        return false;
      }
    }
    if (op == getfield or op == getstatic) {
      if (objectField) {
        if (not m->push(FactMaybe, NoTag)) {
          return false;
        }
      } else if (words == 2) {
        if (not m->pushCat2()) {
          return false;
        }
      } else if (not m->push(FactVal, NoTag)) {
        return false;
      }
    }
    break;
  }

  case invokevirtual:
  case invokespecial:
  case invokestatic:
  case invokeinterface: {
    if ((effect.flags & EffHas) == 0) {
      return false;
    }
    unsigned slots = effect.argSlots;
    if (op == invokespecial and slots > 0 and m->sp >= slots) {
      passRecord(p, ip, m->stack[m->sp - slots].fact);
    }
    m->degrade();
    if (not m->popSlots(slots)) {
      return false;
    }
    unsigned rc = effect.fieldCode;
    if (rc == VoidField) {
      break;
    }
    if (rc == ObjectField) {
      if (not m->push(FactMaybe, NoTag)) {
        return false;
      }
    } else if (osfWords(rc) == 2) {
      if (not m->pushCat2()) {
        return false;
      }
    } else if (not m->push(FactVal, NoTag)) {
      return false;
    }
    break;
  }

  case new_: {
    if ((effect.flags & EffHas) == 0) {
      return false;
    }
    m->degrade();
    uint8_t fact = (effect.flags & EffFresh) ? FactFresh : FactNonNull;
    if (not m->push(fact, NoTag)) {
      return false;
    }
    break;
  }

  case newarray:
  case anewarray: {
    m->degrade();
    if (not m->popInt() or not m->push(FactFresh, NoTag)) {
      return false;
    }
    break;
  }

  case multianewarray: {
    // The outer array is published before the children are allocated,
    // so a collection inside the helper can tenure it.
    m->degrade();
    unsigned dims = p->bytes[ip + 3];
    if (dims == 0) {
      return false;
    }
    for (unsigned d = 0; d < dims; ++d) {
      if (not m->popInt()) {
        return false;
      }
    }
    if (not m->push(FactNonNull, NoTag)) {
      return false;
    }
    break;
  }

  case checkcast:
    m->degrade();
    if (m->sp == 0 or not osfIsRef(m->stack[m->sp - 1].fact)) {
      if (m->sp == 0 or m->stack[m->sp - 1].fact != FactTop) {
        return false;
      }
      m->stack[m->sp - 1].fact = FactMaybe;
      m->stack[m->sp - 1].tag = NoTag;
    }
    break;

  case instanceof: {
    m->degrade();
    Slot s;
    if (not m->popRef(&s) or not m->push(FactVal, NoTag)) {
      return false;
    }
    break;
  }

  case monitorenter:
  case monitorexit: {
    m->degrade();
    Slot s;
    if (not m->popRef(&s)) {
      return false;
    }
    break;
  }

  case athrow: {
    m->degrade();
    Slot s;
    return m->popRef(&s);
  }

  case ireturn:
  case freturn:
    return m->popInt();

  case lreturn:
  case dreturn:
    return m->popCat2();

  case areturn: {
    Slot s;
    return m->popRef(&s);
  }

  case return_:
    return true;

  case ifeq:
  case ifne:
  case iflt:
  case ifge:
  case ifgt:
  case ifle:
  case if_icmpeq:
  case if_icmpne:
  case if_icmplt:
  case if_icmpge:
  case if_icmpgt:
  case if_icmple:
  case ifnull:
  case ifnonnull:
  case if_acmpeq:
  case if_acmpne:
  case goto_:
  case goto_w: {
    unsigned target = 0;
    int offset = (op == goto_w) ? osfReadS32(p->bytes, ip + 1)
                                : osfReadS16(p->bytes, ip + 1);
    if (not osfTarget(ip, offset, p->length, &target)) {
      return false;
    }
    int refineLocal = -1;
    bool nullOnBranch = false;
    if (op == ifnull or op == ifnonnull) {
      Slot s;
      if (not m->popRef(&s)) {
        return false;
      }
      if (s.tag) {
        refineLocal = static_cast<int>(s.tag - 1);
        nullOnBranch = op == ifnull;
      }
    } else if (op == if_acmpeq or op == if_acmpne) {
      Slot a;
      Slot b;
      if (not m->popRef(&a) or not m->popRef(&b)) {
        return false;
      }
      bool nullOnTrue = false;
      int tagged = passNullTag(m, a, b, &nullOnTrue);
      if (tagged >= 0) {
        refineLocal = tagged;
        nullOnBranch = (op == if_acmpeq) ? nullOnTrue : not nullOnTrue;
      }
    } else if (op == if_icmpeq or op == if_icmpne or op == if_icmplt
               or op == if_icmpge or op == if_icmpgt or op == if_icmple) {
      if (not m->popInt() or not m->popInt()) {
        return false;
      }
    } else if (op != goto_ and op != goto_w) {
      if (not m->popInt()) {
        return false;
      }
    }
    // The poll sits before the condition, so both successors lose Fresh.
    // target <= next matches compile.cpp: a jump to the next bytecode
    // is included.
    if (target <= next) {
      m->degrade();
    }
    bool fall = op != goto_ and op != goto_w;
    if (refineLocal >= 0) {
      FactMachine saved = *m;
      memcpy(edgeLocal, m->local, p->maxLocals * sizeof(Slot));
      if (m->sp) {
        memcpy(edgeStack, m->stack, m->sp * sizeof(Slot));
      }
      passRefine(m, refineLocal, nullOnBranch);
      if (not passMerge(p, target, m)) {
        return false;
      }
      *m = saved;
      memcpy(m->local, edgeLocal, p->maxLocals * sizeof(Slot));
      if (m->sp) {
        memcpy(m->stack, edgeStack, m->sp * sizeof(Slot));
      }
      if (fall) {
        passRefine(m, refineLocal, not nullOnBranch);
        if (not passMerge(p, next, m)) {
          return false;
        }
      }
      return true;
    }
    if (not passMerge(p, target, m)) {
      return false;
    }
    if (fall and not passMerge(p, next, m)) {
      return false;
    }
    return true;
  }

  default:
    return false;
  }

  return passMerge(p, next, m);
}

bool osfScan(const uint8_t* bytes, unsigned length, uint8_t* isStart)
{
  unsigned ip = 0;
  while (ip < length) {
    isStart[ip] = 1;
    unsigned op = bytes[ip];
    if (osfForbidden(op)) {
      return false;
    }
    if (op == tableswitch or op == lookupswitch) {
      unsigned end = 0;
      unsigned count = 0;
      if (not osfSwitch(bytes, length, ip, &end, &count, 0) or end <= ip) {
        return false;
      }
      ip = end;
      continue;
    }
    unsigned len = 0;
    if (not osfSimpleLength(op, &len) or ip + len > length) {
      return false;
    }
    ip += len;
  }
  return ip == length;
}

}  // namespace

uint8_t* ObjectStoreFacts::analyze(Thread* t, Zone* zone, GcMethod* method)
{
  if (method == 0 or method->code() == 0) {
    return 0;
  }
  GcCode* code = method->code();
  unsigned length = code->length();
  unsigned maxStack = code->maxStack();
  unsigned maxLocals = code->maxLocals();
  if (length == 0 or length > 65535 or maxStack > 65535 or maxLocals > 65535) {
    return 0;
  }
  uint64_t slots = static_cast<uint64_t>(length)
                   * (static_cast<uint64_t>(maxLocals) + maxStack);
  if (slots > OsfCap) {
    return 0;
  }

  unsigned stride = maxLocals + maxStack;
  uint8_t* bytes = static_cast<uint8_t*>(osfAlloc(zone, length ? length : 1));
  uint8_t* isStart = static_cast<uint8_t*>(osfAlloc(zone, length ? length : 1));
  Effect* effects = static_cast<Effect*>(
      osfAlloc(zone, (length ? length : 1) * sizeof(Effect)));
  unsigned* spOf = static_cast<unsigned*>(
      osfAlloc(zone, (length ? length : 1) * sizeof(unsigned)));
  uint8_t* reach = static_cast<uint8_t*>(osfAlloc(zone, length ? length : 1));
  uint8_t* inQ = static_cast<uint8_t*>(osfAlloc(zone, length ? length : 1));
  uint8_t* out = static_cast<uint8_t*>(osfAlloc(zone, length ? length : 1));
  uint8_t* seen = static_cast<uint8_t*>(osfAlloc(zone, length ? length : 1));
  uint16_t* queue = static_cast<uint16_t*>(
      osfAlloc(zone, (length ? length : 1) * sizeof(uint16_t)));
  unsigned* switches = static_cast<unsigned*>(
      osfAlloc(zone, (length ? length : 1) * sizeof(unsigned)));
  Slot* states = stride ? static_cast<Slot*>(
                              osfAlloc(zone, length * stride * sizeof(Slot)))
                        : 0;
  Slot* entry = maxLocals
                    ? static_cast<Slot*>(osfAlloc(zone, maxLocals * sizeof(Slot)))
                    : 0;

  GcExceptionHandlerTable* table
      = cast<GcExceptionHandlerTable>(t, code->exceptionHandlerTable());
  unsigned handlerCount = table ? table->length() : 0;
  if (handlerCount > length) {
    return 0;
  }
  Handler* handlers = 0;
  if (handlerCount) {
    handlers = static_cast<Handler*>(
        osfAlloc(zone, handlerCount * sizeof(Handler)));
  }

  // Zone allocation can collect. Copy the bytecode and the handler
  // bounds after that, then read pool types into plain integers.
  PROTECT(t, method);
  code = method->code();
  PROTECT(t, code);
  memcpy(bytes, code->body().begin(), length);
  if (handlerCount) {
    table = cast<GcExceptionHandlerTable>(t, code->exceptionHandlerTable());
    if (table == 0 or table->length() != handlerCount) {
      return 0;
    }
    for (unsigned i = 0; i < handlerCount; ++i) {
      uint64_t eh = table->body()[i];
      handlers[i].start = static_cast<uint16_t>(exceptionHandlerStart(eh));
      handlers[i].end = static_cast<uint16_t>(exceptionHandlerEnd(eh));
      handlers[i].ip = static_cast<uint16_t>(exceptionHandlerIp(eh));
    }
  }

  if (not osfScan(bytes, length, isStart)) {
    return 0;
  }

  GcSingleton* pool = code->pool();
  PROTECT(t, pool);
  for (unsigned ip = 0; ip < length; ++ip) {
    if (not isStart[ip]) {
      continue;
    }
    unsigned op = bytes[ip];
    bool need = op == ldc or op == ldc_w or op == getfield or op == putfield
                or op == getstatic or op == putstatic or op == invokevirtual
                or op == invokespecial or op == invokestatic
                or op == invokeinterface or op == new_;
    if (not need) {
      continue;
    }
    unsigned index = 0;
    if (op == ldc) {
      index = bytes[ip + 1];
    } else {
      index = osfReadU16(bytes, ip + 1);
    }
    if (index == 0 or index > poolSize(t, pool)) {
      return 0;
    }
    unsigned at = index - 1;
    if (not singletonIsObject(t, pool, at)
        and op != ldc and op != ldc_w) {
      return 0;
    }
    object o = singletonIsObject(t, pool, at) ? singletonObject(t, pool, at)
                                              : 0;
    bool ok = false;
    if (op == ldc or op == ldc_w) {
      ok = osfLdcEffect(t, pool, at, effects + ip);
    } else if (op == new_) {
      ok = osfNewEffect(t, o, effects + ip);
    } else if (op == getfield or op == putfield or op == getstatic
               or op == putstatic) {
      ok = o and osfFieldEffect(t, o, op, effects + ip);
    } else {
      ok = o and osfMethodEffect(t, o, op != invokestatic, effects + ip);
    }
    if (not ok) {
      return 0;
    }
  }

  if (entry) {
    unsigned local = 0;
    if ((method->flags() & ACC_STATIC) == 0) {
      if (local >= maxLocals) {
        return 0;
      }
      entry[local].fact = FactNonNull;
      ++local;
    }
    GcByteArray* spec = method->spec();
    if (spec == 0) {
      return 0;
    }
    const int8_t* s = spec->body().begin();
    unsigned n = spec->length();
    if (n == 0 or s[0] != '(') {
      return 0;
    }
    unsigned i = 1;
    while (i < n and s[i] != ')') {
      unsigned fc = 0;
      if (not osfParseType(s, n, &i, &fc)) {
        return 0;
      }
      unsigned words = osfWords(fc);
      if (local + words > maxLocals) {
        return 0;
      }
      if (fc == ObjectField) {
        entry[local].fact = FactMaybe;
        ++local;
      } else if (words == 2) {
        entry[local].fact = FactVal;
        entry[local + 1].fact = FactVal2;
        local += 2;
      } else {
        entry[local].fact = FactVal;
        ++local;
      }
    }
  }

  Pass pass;
  memset(&pass, 0, sizeof(pass));
  pass.bytes = bytes;
  pass.length = length;
  pass.maxLocals = maxLocals;
  pass.maxStack = maxStack;
  pass.stride = stride;
  pass.states = states;
  pass.spOf = spOf;
  pass.reach = reach;
  pass.inQ = inQ;
  pass.isStart = isStart;
  pass.queue = queue;
  pass.out = out;
  pass.seen = seen;
  pass.effects = effects;
  pass.switches = switches;
  pass.debug = debug::enabled();

  Slot* scratchLocal = maxLocals
                           ? static_cast<Slot*>(
                                 osfAlloc(zone, maxLocals * sizeof(Slot)))
                           : 0;
  Slot* scratchStack = maxStack ? static_cast<Slot*>(
                                      osfAlloc(zone, maxStack * sizeof(Slot)))
                                : 0;
  Slot* edgeLocal = maxLocals ? static_cast<Slot*>(
                                    osfAlloc(zone, maxLocals * sizeof(Slot)))
                              : 0;
  Slot* edgeStack = maxStack ? static_cast<Slot*>(
                                   osfAlloc(zone, maxStack * sizeof(Slot)))
                             : 0;

  // These copies happen after the pool snapshot. They do not touch the
  // Java heap, so a collection here cannot invalidate bytes or effects.
  if (not isStart[0]) {
    return 0;
  }
  pass.reach[0] = 1;
  pass.spOf[0] = 0;
  if (maxLocals) {
    memcpy(passSlots(&pass, 0), entry, maxLocals * sizeof(Slot));
  }
  if (not passEnqueue(&pass, 0)) {
    return 0;
  }

  for (unsigned h = 0; h < handlerCount; ++h) {
    unsigned hip = handlers[h].ip;
    if (hip >= length or not isStart[hip] or maxStack < 1) {
      return 0;
    }
    FactMachine handler;
    handler.local = scratchLocal;
    handler.stack = scratchStack;
    handler.maxLocals = maxLocals;
    handler.maxStack = maxStack;
    handler.sp = 1;
    for (unsigned i = 0; i < maxLocals; ++i) {
      scratchLocal[i].fact = FactTop;
      scratchLocal[i].tag = NoTag;
    }
    scratchStack[0].fact = FactNonNull;
    scratchStack[0].tag = NoTag;
    if (not passMerge(&pass, hip, &handler)) {
      return 0;
    }
  }

  unsigned limit = length * 8u * (stride + 1u) + 8u;
  while (pass.qCount) {
    if (++pass.steps > limit) {
      return 0;
    }
    unsigned ip = pass.queue[pass.qh];
    pass.qh = (pass.qh + 1) % length;
    --pass.qCount;
    pass.inQ[ip] = 0;
    FactMachine m;
    passCopy(&pass, ip, &m, scratchLocal, scratchStack);
    if (not passTransfer(&pass, ip, &m, edgeLocal, edgeStack)) {
      return 0;
    }
  }

  for (unsigned i = 0; i < length; ++i) {
    if (out[i]) {
      return out;
    }
  }
  return 0;
}

}  // namespace vm
