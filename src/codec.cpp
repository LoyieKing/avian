/* Copyright (c) 2008-2015, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

// ByteFun state graphs. Each class publishes two stubs in
// VMClass.serializeThunk / deserializeThunk. The serialize walk does not
// allocate on the Java heap, so object pointers stay put. CaptureState
// keeps that buffer. SerializeGraph still wraps it in one byte[].

#include "avian/machine.h"

#include <pthread.h>
#include <setjmp.h>
#include <sys/mman.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace vm {

namespace {

const unsigned kMaxDepth = 4096;
const unsigned kEnumFlag = 1 << 14;
const unsigned kHopCap = 1 << 20;
const unsigned kSlabBytes = 1 << 20;
const unsigned kStubBytes = 32;

enum Tag {
  T_NULL = 0,
  T_BOOL = 1,
  T_INT = 2,
  T_LONG = 3,
  T_FLOAT = 4,
  T_DOUBLE = 5,
  T_STRING = 6,
  T_ENUM = 7,
  T_CLASS = 8,
  T_ARRAY = 9,
  T_LIST = 10,
  T_SET = 11,
  T_MAP = 12,
  T_OBJECT = 13,
  T_BACKREF = 14
};

enum Shape {
  SH_POJO = 0,
  SH_STRING,
  SH_CLASS,
  SH_ENUM,
  SH_ARRAY,
  SH_WRAPPER,
  SH_SINGLETON,
  SH_HASHSET,
  SH_LINKED,
  SH_HASHMAP,
  SH_ARRAYLIST,
  SH_PLAIN,
  SH_EMPTY,
  SH_DEQUE,
  SH_CHM,
  SH_DELEGATE,
  SH_TREE,
  SH_SUBLIST,
  SH_UNKNOWN
};

enum CollectMode {
  MODE_ELEMS = 0,
  MODE_PAIRS = 1,
  MODE_VALUES = 2,
  MODE_CELLS = 3
};

struct FieldPlan {
  char* owner;
  uint32_t ownerN;
  char* name;
  uint32_t nameN;
  char* spec;
  uint32_t specN;
  int offset;
  uint8_t code;
};

struct Plan {
  char* name;
  uint32_t nameN;
  uint8_t shape;
  uint8_t tag;
  uint8_t prim;
  int fieldCount;
  FieldPlan* fields;
  int innerOff;
  int elementOff;
  int mapOff;
  int frontOff;
  int sizeOff;
  int arrayOff;
  int enumNameOff;
  char* enumName;
  uint32_t enumNameN;
  char* component;
  uint32_t componentN;
  int vmOff;
  int contentOff;
  int listOff;
  int collectionOff;
  int parentOff;
  int setOff;
  int dataOff;
  int startOff;
  int endOff;
  int offsetOff;
  int soleOff;
  int delegateOff;
};

struct Buf {
  uint8_t* data;
  uint32_t len;
  uint32_t cap;
  Buf* next;
  int heap;
};

struct IdSlot {
  object key;
  int32_t id;
};

struct IdTab {
  IdSlot* slots;
  uint32_t cap;
  uint32_t count;
  IdTab* parent;
  IdTab* next;
  int heap;
};

struct Piece {
  uint8_t* key;
  uint32_t keyN;
  uint8_t* val;
  uint32_t valN;
  object keyObj;
  object valObj;
  Piece* next;
};

struct Scratch {
  void* p;
  Scratch* next;
};

struct OffCache {
  GcClass* cls;
  int a;
  int b;
  int c;
  OffCache* next;
};

struct Walk {
  Thread* t;
  Buf* out;
  IdTab* ids;
  int depth;
  GcClass* mapC;
  GcClass* colC;
  GcClass* setC;
  Buf* bufs;
  IdTab* idList;
  Piece* pieces;
  Scratch* scratches;
  OffCache* cells;
};

struct Slice {
  const uint8_t* p;
  uint32_t n;
};

struct Val {
  uint8_t tag;
  object obj;
  uint64_t bits;
};

enum {
  K_BOOL = 0,
  K_BYTE,
  K_CHAR,
  K_SHORT,
  K_INT,
  K_LONG,
  K_FLOAT,
  K_DOUBLE,
  K_EMPTY_LIST,
  K_EMPTY_SET,
  K_EMPTY_MAP,
  K_CACHE_N
};

struct Dec {
  Thread* t;
  const uint8_t* p;
  const uint8_t* end;
  object ids;
  uint32_t idCount;
  uint32_t idCap;
  object current;
  object cache;
  int depth;
};

__thread Walk* g_walk = 0;
__thread Dec* g_dec = 0;
__thread GcClass* g_mapC = 0;
__thread GcClass* g_colC = 0;
__thread GcClass* g_setC = 0;
__thread int g_hot = 0;
__thread int g_keep = 0;
__thread uint8_t* g_kept = 0;
__thread uint32_t g_kept_n = 0;
GcClass* g_slot_map = 0;
GcClass* g_slot_col = 0;
GcClass* g_slot_set = 0;

pthread_mutex_t g_stubMu = PTHREAD_MUTEX_INITIALIZER;
uint8_t* g_slab = 0;
unsigned g_slabUsed = 0;

void release_walk(Walk* w);
void NO_RETURN fail(Thread* t, int oom, const char* msg);
void write_value(Walk* w, object obj);
void write_planned(Walk* w, object self, Plan* plan);
void ensure_fn(Thread* t, GcClass* class_);
GcByteArray* entry(Thread* t, object self, Plan* plan);
object dentry(Thread* t, Plan* plan);
Val read_value(Dec* d);

void drop_kept()
{
  if (g_kept) {
    free(g_kept);
    g_kept = 0;
    g_kept_n = 0;
  }
}

void poison()
{
  g_hot = 0;
  g_mapC = 0;
  g_colC = 0;
  g_setC = 0;
  g_keep = 0;
  drop_kept();
}

void cpu_pause()
{
#ifdef ARCH_x86_64
  __asm__ __volatile__("pause");
#else
  __asm__ __volatile__("" ::: "memory");
#endif
}

GcClass* raw_type(Thread* t, unsigned id)
{
  return reinterpret_cast<GcClass*>(t->m->types->body()[id]);
}

void NO_RETURN rethrow(Thread* t)
{
  GcThrowable* e = t->exception;
  t->exception = 0;
  release_walk(g_walk);
  g_walk = 0;
  g_dec = 0;
  poison();
  if (e) {
    throw_(t, e);
  }
  throwNew(t, GcIllegalArgumentException::Type, "%s", "bad state payload");
}

void check_exc(Thread* t)
{
  if (t->exception) {
    rethrow(t);
  }
}

void release_walk(Walk* w)
{
  if (!w) {
    return;
  }
  for (Buf* b = w->bufs; b;) {
    Buf* n = b->next;
    free(b->data);
    b->data = 0;
    if (b->heap) {
      free(b);
    }
    b = n;
  }
  w->bufs = 0;
  for (IdTab* tab = w->idList; tab;) {
    IdTab* n = tab->next;
    free(tab->slots);
    tab->slots = 0;
    if (tab->heap) {
      free(tab);
    }
    tab = n;
  }
  w->idList = 0;
  while (w->pieces) {
    Piece* p = w->pieces;
    w->pieces = p->next;
    free(p->key);
    free(p->val);
    free(p);
  }
  for (Scratch* s = w->scratches; s;) {
    Scratch* n = s->next;
    free(s->p);
    s->p = 0;
    s = n;
  }
  w->scratches = 0;
  while (w->cells) {
    OffCache* c = w->cells;
    w->cells = c->next;
    free(c);
  }
  w->out = 0;
  w->ids = 0;
}

void NO_RETURN fail(Thread* t, int oom, const char* msg)
{
  char buf[512];
  if (!msg) {
    msg = "bad state payload";
  }
  unsigned i = 0;
  for (; msg[i] && i + 1 < sizeof buf; ++i) {
    buf[i] = msg[i];
  }
  buf[i] = 0;
  release_walk(g_walk);
  g_walk = 0;
  g_dec = 0;
  poison();
  if (oom) {
    throwNew(t, GcOutOfMemoryError::Type, "%s", buf);
  }
  throwNew(t, GcIllegalArgumentException::Type, "%s", buf);
}

void* xmalloc(Thread* t, size_t n)
{
  if (n == 0) {
    n = 1;
  }
  void* p = malloc(n);
  if (!p) {
    fail(t, 1, "OutOfMemoryError");
  }
  return p;
}

void* xrealloc(Thread* t, void* p, size_t n)
{
  if (n == 0) {
    n = 1;
  }
  void* q = realloc(p, n);
  if (!q) {
    fail(t, 1, "OutOfMemoryError");
  }
  return q;
}

unsigned bytes_n(GcByteArray* a)
{
  if (!a) {
    return 0;
  }
  unsigned n = static_cast<unsigned>(a->length());
  if (n && a->body().begin()[n - 1] == 0) {
    --n;
  }
  return n;
}

const uint8_t* bytes_p(GcByteArray* a)
{
  if (!a || a->length() == 0) {
    return reinterpret_cast<const uint8_t*>("");
  }
  return reinterpret_cast<const uint8_t*>(a->body().begin());
}

bool raw_is(GcByteArray* a, const char* lit)
{
  unsigned n = bytes_n(a);
  unsigned m = static_cast<unsigned>(strlen(lit));
  if (n != m) {
    return false;
  }
  return n == 0 || memcmp(bytes_p(a), lit, n) == 0;
}

bool class_named(GcClass* c, const char* internal)
{
  return c && raw_is(c->name(), internal);
}

bool next_unit(const uint8_t*& cur, const uint8_t* end, uint32_t* cp)
{
  if (cur >= end) {
    return false;
  }
  uint8_t a = *cur++;
  if ((a & 0x80) == 0) {
    *cp = a;
    return true;
  }
  if ((a & 0xE0) == 0xC0) {
    if (cur >= end) {
      return false;
    }
    uint8_t b = *cur++;
    *cp = (static_cast<uint32_t>(a & 0x1F) << 6) | (b & 0x3F);
    return true;
  }
  if (cur + 1 >= end) {
    return false;
  }
  uint8_t b = *cur++;
  uint8_t c = *cur++;
  *cp = (static_cast<uint32_t>(a & 0x0F) << 12)
        | (static_cast<uint32_t>(b & 0x3F) << 6) | (c & 0x3F);
  return true;
}

bool next_cp(const uint8_t*& cur, const uint8_t* end, uint32_t* cp)
{
  if (!next_unit(cur, end, cp)) {
    return false;
  }
  if (*cp >= 0xD800 && *cp <= 0xDBFF && cur < end) {
    const uint8_t* save = cur;
    uint32_t low;
    if (!next_unit(cur, end, &low)) {
      cur = save;
      return true;
    }
    if (low >= 0xDC00 && low <= 0xDFFF) {
      *cp = 0x10000 + (((*cp - 0xD800) << 10) | (low - 0xDC00));
    } else {
      cur = save;
    }
  }
  return true;
}

bool next_utf8(const uint8_t*& cur, const uint8_t* end, uint32_t* cp)
{
  if (cur >= end) {
    return false;
  }
  unsigned char c = *cur;
  if (c < 0x80) {
    *cp = c;
    ++cur;
    return true;
  }
  if ((c & 0xE0) == 0xC0 && cur + 1 < end) {
    *cp = (static_cast<uint32_t>(c & 0x1F) << 6) | (cur[1] & 0x3F);
    cur += 2;
    return true;
  }
  if ((c & 0xF0) == 0xE0 && cur + 2 < end) {
    *cp = (static_cast<uint32_t>(c & 0x0F) << 12)
          | (static_cast<uint32_t>(cur[1] & 0x3F) << 6) | (cur[2] & 0x3F);
    cur += 3;
    return true;
  }
  if ((c & 0xF8) == 0xF0 && cur + 3 < end) {
    *cp = (static_cast<uint32_t>(c & 0x07) << 18)
          | (static_cast<uint32_t>(cur[1] & 0x3F) << 12)
          | (static_cast<uint32_t>(cur[2] & 0x3F) << 6) | (cur[3] & 0x3F);
    cur += 4;
    return true;
  }
  *cp = 0xFFFD;
  ++cur;
  return true;
}

unsigned emit_utf8(uint8_t* dst, uint32_t cp)
{
  if (cp < 0x80) {
    dst[0] = static_cast<uint8_t>(cp);
    return 1;
  }
  if (cp < 0x800) {
    dst[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    dst[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    dst[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
    dst[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 3;
  }
  dst[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
  dst[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
  dst[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
  dst[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  return 4;
}

unsigned emit_mutf8(uint8_t* dst, uint32_t cp, unsigned* chars)
{
  if (cp >= 0x10000) {
    cp -= 0x10000;
    unsigned n = emit_mutf8(dst, 0xD800 + (cp >> 10), chars);
    return n + emit_mutf8(dst + n, 0xDC00 + (cp & 0x3FF), chars);
  }
  ++*chars;
  if (cp == 0) {
    dst[0] = 0xC0;
    dst[1] = 0x80;
    return 2;
  }
  if (cp < 0x80) {
    dst[0] = static_cast<uint8_t>(cp);
    return 1;
  }
  if (cp < 0x800) {
    dst[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    dst[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 2;
  }
  dst[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
  dst[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
  dst[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  return 3;
}

char* utf8_from_mutf8(Thread* t,
                      const uint8_t* p,
                      unsigned n,
                      uint32_t* outN,
                      int slashToDot)
{
  char* buf = static_cast<char*>(xmalloc(t, static_cast<size_t>(n) + 1));
  uint32_t w = 0;
  const uint8_t* cur = p;
  const uint8_t* end = p + n;
  while (cur < end) {
    uint32_t cp;
    if (!next_cp(cur, end, &cp)) {
      free(buf);
      return 0;
    }
    if (slashToDot && cp == '/') {
      cp = '.';
    }
    uint8_t tmp[4];
    unsigned nb = emit_utf8(tmp, cp);
    memcpy(buf + w, tmp, nb);
    w += nb;
  }
  buf[w] = 0;
  *outN = w;
  return buf;
}

const char* primitive_name(Thread* t, GcClass* c)
{
  if (!c) {
    return 0;
  }
  if (c == raw_type(t, GcJboolean::Type)) return "boolean";
  if (c == raw_type(t, GcJbyte::Type)) return "byte";
  if (c == raw_type(t, GcJchar::Type)) return "char";
  if (c == raw_type(t, GcJshort::Type)) return "short";
  if (c == raw_type(t, GcJint::Type)) return "int";
  if (c == raw_type(t, GcJlong::Type)) return "long";
  if (c == raw_type(t, GcJfloat::Type)) return "float";
  if (c == raw_type(t, GcJdouble::Type)) return "double";
  if (c == raw_type(t, GcJvoid::Type)) return "void";
  return 0;
}

int primitive_code(Thread* t, GcClass* c)
{
  if (!c) {
    return -1;
  }
  if (c == raw_type(t, GcJboolean::Type)) return BooleanField;
  if (c == raw_type(t, GcJbyte::Type)) return ByteField;
  if (c == raw_type(t, GcJchar::Type)) return CharField;
  if (c == raw_type(t, GcJshort::Type)) return ShortField;
  if (c == raw_type(t, GcJint::Type)) return IntField;
  if (c == raw_type(t, GcJlong::Type)) return LongField;
  if (c == raw_type(t, GcJfloat::Type)) return FloatField;
  if (c == raw_type(t, GcJdouble::Type)) return DoubleField;
  return -1;
}

bool is_array_class(GcClass* c)
{
  if (!c) {
    return false;
  }
  if (c->arrayDimensions() > 0) {
    return true;
  }
  GcByteArray* name = c->name();
  return name && name->length() > 0 && name->body().begin()[0] == '[';
}

bool is_enum_class(GcClass* c)
{
  return c && class_named(c->super(), "java/lang/Enum")
         && (c->flags() & kEnumFlag) != 0;
}

bool is_enum_type(GcClass* c)
{
  return is_enum_class(c) || (c && is_enum_class(c->super()));
}

unsigned array_len(object array)
{
  if (!array) {
    return 0;
  }
  uintptr_t n = fieldAtOffset<uintptr_t>(array, BytesPerWord);
  if (n > 0xffffffffu) {
    return 0xffffffffu;
  }
  return static_cast<unsigned>(n);
}

object array_at(object array, unsigned index)
{
  return fieldAtOffset<object>(array, ArrayBody + index * BytesPerWord);
}

bool implements_(GcClass* c, GcClass* iface, int depth)
{
  if (!c || !iface || depth > 64) {
    return false;
  }
  if (c == iface) {
    return true;
  }
  object table = c->interfaceTable();
  if (table) {
    unsigned n = array_len(table);
    unsigned stride = (c->flags() & ACC_INTERFACE) ? 1 : 2;
    if (stride == 0) {
      stride = 1;
    }
    for (unsigned i = 0; i < n; i += stride) {
      GcClass* in = reinterpret_cast<GcClass*>(array_at(table, i));
      if (in == iface || implements_(in, iface, depth + 1)) {
        return true;
      }
    }
  }
  return implements_(c->super(), iface, depth + 1);
}

object field_table(GcClass* c)
{
  return c ? c->fieldTable() : 0;
}

int find_field_off(GcClass* c, const char* name, int code, int walkSuper)
{
  int guard = 0;
  for (GcClass* k = c; k && guard < 64; k = walkSuper ? k->super() : 0, ++guard) {
    object table = field_table(k);
    unsigned n = array_len(table);
    for (unsigned i = 0; i < n; ++i) {
      GcField* f = reinterpret_cast<GcField*>(array_at(table, i));
      if (!f || (f->flags() & ACC_STATIC)) {
        continue;
      }
      if (code >= 0 && f->code() != code) {
        continue;
      }
      if (raw_is(f->name(), name)) {
        return f->offset();
      }
    }
    if (!walkSuper) {
      break;
    }
  }
  return -1;
}

void string_bytes(object obj, const uint8_t*& p, unsigned& n)
{
  GcString* s = reinterpret_cast<GcString*>(obj);
  GcByteArray* data = s->data();
  if (data) {
    p = bytes_p(data);
    n = bytes_n(data);
    return;
  }
  const uint8_t* h = reinterpret_cast<const uint8_t*>(
      static_cast<uintptr_t>(s->unsafe_data()));
  if (!h) {
    p = reinterpret_cast<const uint8_t*>("");
    n = 0;
    return;
  }
  n = (static_cast<unsigned>(h[0]) << 8) | h[1];
  p = h + 2;
}

bool string_equals_utf8(object str, const uint8_t* utf, uint32_t utfN)
{
  if (!str) {
    return false;
  }
  const uint8_t* mp;
  unsigned mn = 0;
  string_bytes(str, mp, mn);
  const uint8_t* a = mp;
  const uint8_t* aEnd = mp + mn;
  const uint8_t* b = utf;
  const uint8_t* bEnd = utf + utfN;
  while (a < aEnd || b < bEnd) {
    if (a >= aEnd || b >= bEnd) {
      return false;
    }
    uint32_t ca;
    uint32_t cb;
    if (!next_cp(a, aEnd, &ca) || !next_utf8(b, bEnd, &cb)) {
      return false;
    }
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

uintptr_t hash_obj(object o)
{
  uintptr_t x = reinterpret_cast<uintptr_t>(o) >> 3;
  x ^= x >> 30;
  x *= static_cast<uintptr_t>(0xbf58476d1ce4e5b9ULL);
  x ^= x >> 27;
  return x;
}

void id_grow(Walk* w, IdTab* tab)
{
  uint32_t cap = tab->cap ? tab->cap * 2 : 16;
  IdSlot* slots
      = static_cast<IdSlot*>(xmalloc(w->t, static_cast<size_t>(cap) * sizeof(IdSlot)));
  memset(slots, 0, static_cast<size_t>(cap) * sizeof(IdSlot));
  uint32_t mask = cap - 1;
  for (uint32_t i = 0; i < tab->cap; ++i) {
    object k = tab->slots[i].key;
    if (!k) {
      continue;
    }
    uint32_t j = static_cast<uint32_t>(hash_obj(k)) & mask;
    while (slots[j].key) {
      j = (j + 1) & mask;
    }
    slots[j].key = k;
    slots[j].id = tab->slots[i].id;
  }
  free(tab->slots);
  tab->slots = slots;
  tab->cap = cap;
}

int id_find(IdTab* tab, object o)
{
  if (!o) {
    return -1;
  }
  for (IdTab* t = tab; t; t = t->parent) {
    if (!t->cap) {
      continue;
    }
    uint32_t mask = t->cap - 1;
    uint32_t i = static_cast<uint32_t>(hash_obj(o)) & mask;
    for (;;) {
      object k = t->slots[i].key;
      if (!k) {
        break;
      }
      if (k == o) {
        return t->slots[i].id;
      }
      i = (i + 1) & mask;
    }
  }
  return -1;
}

void id_add(Walk* w, object o)
{
  IdTab* tab = w->ids;
  if (tab->count * 2 >= tab->cap) {
    id_grow(w, tab);
  }
  uint32_t mask = tab->cap - 1;
  uint32_t i = static_cast<uint32_t>(hash_obj(o)) & mask;
  while (tab->slots[i].key) {
    i = (i + 1) & mask;
  }
  tab->slots[i].key = o;
  tab->slots[i].id = static_cast<int32_t>(tab->count);
  ++tab->count;
}

uint8_t* reserve(Walk* w, uint32_t n)
{
  Buf* b = w->out;
  if (n > 0xffffffffu - b->len) {
    fail(w->t, 1, "OutOfMemoryError");
  }
  if (b->len + n > b->cap) {
    uint32_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + n) {
      if (cap > 0x7fffffffu / 2) {
        fail(w->t, 1, "OutOfMemoryError");
      }
      cap *= 2;
    }
    uint8_t* p = static_cast<uint8_t*>(
        xrealloc(w->t, b->data, cap));
    b->data = p;
    b->cap = cap;
  }
  uint8_t* out = b->data + b->len;
  b->len += n;
  return out;
}

void put_u8(Walk* w, uint8_t v)
{
  *reserve(w, 1) = v;
}

void put_u32(Walk* w, uint32_t v)
{
  uint8_t* p = reserve(w, 4);
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

void put_u64(Walk* w, uint64_t v)
{
  uint8_t* p = reserve(w, 8);
  for (int s = 0; s < 8; ++s) {
    p[s] = static_cast<uint8_t>(v >> (56 - 8 * s));
  }
}

void put_mem(Walk* w, const void* p, uint32_t n)
{
  put_u32(w, n);
  if (n) {
    memcpy(reserve(w, n), p, n);
  }
}

void put_cstr(Walk* w, const char* s)
{
  put_mem(w, s, static_cast<uint32_t>(strlen(s)));
}

int plain_ascii(const uint8_t* p, unsigned n)
{
  for (unsigned i = 0; i < n; ++i) {
    if (p[i] == 0 || p[i] >= 0x80) {
      return 0;
    }
  }
  return 1;
}

void put_mutf8_utf8(Walk* w, const uint8_t* p, unsigned n)
{
  if (plain_ascii(p, n)) {
    put_mem(w, p, n);
    return;
  }
  uint32_t hole = w->out->len;
  reserve(w, 4);
  uint32_t start = w->out->len;
  const uint8_t* cur = p;
  const uint8_t* end = p + n;
  while (cur < end) {
    uint32_t cp;
    if (!next_cp(cur, end, &cp)) {
      fail(w->t, 0, "bad state payload");
    }
    uint8_t tmp[4];
    unsigned nb = emit_utf8(tmp, cp);
    memcpy(reserve(w, nb), tmp, nb);
  }
  uint32_t len = w->out->len - start;
  w->out->data[hole] = static_cast<uint8_t>(len >> 24);
  w->out->data[hole + 1] = static_cast<uint8_t>(len >> 16);
  w->out->data[hole + 2] = static_cast<uint8_t>(len >> 8);
  w->out->data[hole + 3] = static_cast<uint8_t>(len);
}

void put_dotted_class(Walk* w, GcClass* c)
{
  const char* prim = primitive_name(w->t, c);
  if (prim) {
    put_cstr(w, prim);
    return;
  }
  unsigned n = bytes_n(c ? c->name() : 0);
  const uint8_t* p = bytes_p(c ? c->name() : 0);
  if (plain_ascii(p, n)) {
    uint32_t hole = w->out->len;
    reserve(w, 4);
    uint8_t* dst = reserve(w, n);
    for (unsigned i = 0; i < n; ++i) {
      dst[i] = p[i] == '/' ? static_cast<uint8_t>('.') : p[i];
    }
    w->out->data[hole] = static_cast<uint8_t>(n >> 24);
    w->out->data[hole + 1] = static_cast<uint8_t>(n >> 16);
    w->out->data[hole + 2] = static_cast<uint8_t>(n >> 8);
    w->out->data[hole + 3] = static_cast<uint8_t>(n);
    return;
  }
  uint32_t hole = w->out->len;
  reserve(w, 4);
  uint32_t start = w->out->len;
  const uint8_t* cur = p;
  const uint8_t* end = p + n;
  while (cur < end) {
    uint32_t cp;
    if (!next_cp(cur, end, &cp)) {
      fail(w->t, 0, "bad state payload");
    }
    if (cp == '/') {
      cp = '.';
    }
    uint8_t tmp[4];
    unsigned nb = emit_utf8(tmp, cp);
    memcpy(reserve(w, nb), tmp, nb);
  }
  uint32_t len = w->out->len - start;
  w->out->data[hole] = static_cast<uint8_t>(len >> 24);
  w->out->data[hole + 1] = static_cast<uint8_t>(len >> 16);
  w->out->data[hole + 2] = static_cast<uint8_t>(len >> 8);
  w->out->data[hole + 3] = static_cast<uint8_t>(len);
}

void put_plan_str(Walk* w, const char* s, uint32_t n)
{
  put_mem(w, s ? s : "", s ? n : 0);
}

void free_plan(Plan* plan)
{
  if (!plan) {
    return;
  }
  free(plan->name);
  free(plan->enumName);
  free(plan->component);
  if (plan->fields) {
    for (int i = 0; i < plan->fieldCount; ++i) {
      free(plan->fields[i].owner);
      free(plan->fields[i].name);
      free(plan->fields[i].spec);
    }
    free(plan->fields);
  }
  free(plan);
}

void add_pojo_field(Thread* t, Plan* plan, FieldPlan field)
{
  if (plan->fieldCount == 0 || (plan->fieldCount & (plan->fieldCount - 1)) == 0) {
    int cap = plan->fieldCount ? plan->fieldCount * 2 : 4;
    plan->fields = static_cast<FieldPlan*>(xrealloc(
        t, plan->fields, static_cast<size_t>(cap) * sizeof(FieldPlan)));
  }
  plan->fields[plan->fieldCount++] = field;
}

bool ref_array_spec(GcField* f)
{
  if (!f || f->code() != ObjectField) {
    return false;
  }
  GcByteArray* spec = f->spec();
  if (bytes_n(spec) < 2) {
    return false;
  }
  const uint8_t* p = bytes_p(spec);
  return p[0] == '[' && (p[1] == 'L' || p[1] == '[');
}

char* dup_field_text(Thread* t, GcByteArray* a, uint32_t* n, int slash)
{
  char* s = utf8_from_mutf8(t, bytes_p(a), bytes_n(a), n, slash);
  if (!s) {
    fail(t, 0, "bad state payload");
  }
  return s;
}

bool name_ends(const char* name, const char* lit)
{
  if (!name || !lit) {
    return false;
  }
  size_t n = strlen(name);
  size_t m = strlen(lit);
  if (n < m || memcmp(name + (n - m), lit, m) != 0) {
    return false;
  }
  return n == m || name[n - m - 1] == '$' || name[n - m - 1] == '.';
}

int collect_mode(const Plan* plan, int pairs)
{
  if (pairs) {
    return MODE_PAIRS;
  }
  if (plan && name_ends(plan->name, "EntrySet")) {
    return MODE_CELLS;
  }
  if (plan && name_ends(plan->name, "Values")) {
    return MODE_VALUES;
  }
  return MODE_ELEMS;
}

Plan* build_plan(Thread* t,
                 GcClass* class_,
                 GcClass* mapC,
                 GcClass* colC,
                 GcClass* setC)
{
  Plan* plan = static_cast<Plan*>(xmalloc(t, sizeof(Plan)));
  memset(plan, 0, sizeof(Plan));
  plan->innerOff = -1;
  plan->elementOff = -1;
  plan->mapOff = -1;
  plan->frontOff = -1;
  plan->sizeOff = -1;
  plan->arrayOff = -1;
  plan->enumNameOff = -1;
  plan->contentOff = -1;
  plan->listOff = -1;
  plan->collectionOff = -1;
  plan->parentOff = -1;
  plan->setOff = -1;
  plan->dataOff = -1;
  plan->startOff = -1;
  plan->endOff = -1;
  plan->offsetOff = -1;
  plan->soleOff = -1;
  plan->delegateOff = -1;
  plan->vmOff = static_cast<int>(JclassVmClass);
  plan->prim = ObjectField;

  const char* prim = primitive_name(t, class_);
  if (prim) {
    plan->name = static_cast<char*>(xmalloc(t, strlen(prim) + 1));
    memcpy(plan->name, prim, strlen(prim) + 1);
    plan->nameN = static_cast<uint32_t>(strlen(prim));
  } else {
    plan->name = dup_field_text(t, class_->name(), &plan->nameN, 1);
  }

  if (class_ == raw_type(t, GcString::Type)) {
    plan->shape = SH_STRING;
  } else if (class_ == raw_type(t, GcJclass::Type)) {
    plan->shape = SH_CLASS;
  } else if (is_enum_type(class_)) {
    plan->shape = SH_ENUM;
    GcClass* enumC = is_enum_class(class_) ? class_ : class_->super();
    plan->enumName = dup_field_text(t, enumC->name(), &plan->enumNameN, 1);
    int guard = 0;
    for (GcClass* k = class_; k && guard < 64; k = k->super(), ++guard) {
      object table = field_table(k);
      unsigned n = array_len(table);
      for (unsigned i = 0; i < n; ++i) {
        GcField* f = reinterpret_cast<GcField*>(array_at(table, i));
        if (!f || (f->flags() & ACC_STATIC) || f->code() != ObjectField) {
          continue;
        }
        if (raw_is(f->name(), "name")
            && class_named(f->class_(), "java/lang/Enum")) {
          plan->enumNameOff = f->offset();
        }
      }
    }
  } else if (is_array_class(class_)) {
    plan->shape = SH_ARRAY;
    GcClass* comp = class_->arrayElementClass();
    int code = primitive_code(t, comp);
    if (code >= 0) {
      const char* cn = primitive_name(t, comp);
      plan->component = static_cast<char*>(xmalloc(t, strlen(cn) + 1));
      memcpy(plan->component, cn, strlen(cn) + 1);
      plan->componentN = static_cast<uint32_t>(strlen(cn));
      plan->prim = static_cast<uint8_t>(code);
    } else if (comp) {
      plan->component = dup_field_text(t, comp->name(), &plan->componentN, 1);
      plan->prim = ObjectField;
    } else {
      plan->component = static_cast<char*>(xmalloc(t, 1));
      plan->component[0] = 0;
      plan->componentN = 0;
    }
  }

  int innerOff = -1;
  int elementOff = -1;
  int mapOff = -1;
  int frontOff = -1;
  int sizeOff = -1;
  int namedOff = -1;
  int namedRank = 0;
  int anyArray = -1;
  int refCount = 0;
  int refArrayCount = 0;
  int contentOff = -1;
  int listOff = -1;
  int collectionOff = -1;
  int parentOff = -1;
  int setOff = -1;
  int dataOff = -1;
  int startOff = -1;
  int endOff = -1;
  int offsetOff = -1;
  int soleOff = -1;
  GcClass* objectC = raw_type(t, GcJobject::Type);
  int guard = 0;
  for (GcClass* k = class_; k && k != objectC && guard < 64;
       k = k->super(), ++guard) {
    object table = field_table(k);
    unsigned n = array_len(table);
    uint32_t ownerN = 0;
    char* owner = 0;
    int ownerReady = 0;
    for (unsigned i = 0; i < n; ++i) {
      GcField* f = reinterpret_cast<GcField*>(array_at(table, i));
      if (!f || (f->flags() & ACC_STATIC)) {
        continue;
      }
      int transientBit = (f->flags() & ACC_TRANSIENT) != 0;
      if (f->code() == ObjectField) {
        ++refCount;
        if (refCount == 1) soleOff = f->offset();
        else soleOff = -1;
        if (ref_array_spec(f)) {
          ++refArrayCount;
          if (anyArray < 0) {
            anyArray = f->offset();
          }
          int rank = 0;
          if (raw_is(f->name(), "array")) rank = 3;
          else if (raw_is(f->name(), "elementData")) rank = 2;
          else if (raw_is(f->name(), "a")) rank = 1;
          if (rank > namedRank) {
            namedRank = rank;
            namedOff = f->offset();
          }
        }
        if (innerOff < 0 && raw_is(f->name(), "inner")) innerOff = f->offset();
        if (elementOff < 0 && raw_is(f->name(), "element"))
          elementOff = f->offset();
        if (mapOff < 0 && raw_is(f->name(), "map")) mapOff = f->offset();
        if (frontOff < 0 && raw_is(f->name(), "front")) frontOff = f->offset();
        if (contentOff < 0 && raw_is(f->name(), "content")) contentOff = f->offset();
        if (listOff < 0 && raw_is(f->name(), "list")) listOff = f->offset();
        if (collectionOff < 0 && raw_is(f->name(), "collection"))
          collectionOff = f->offset();
        if (parentOff < 0 && raw_is(f->name(), "parent")) parentOff = f->offset();
        if (setOff < 0 && raw_is(f->name(), "set")) setOff = f->offset();
        if (dataOff < 0 && raw_is(f->name(), "dataArray") && ref_array_spec(f))
          dataOff = f->offset();
      } else if (f->code() == IntField) {
        if (sizeOff < 0 && raw_is(f->name(), "size")) sizeOff = f->offset();
        if (startOff < 0 && raw_is(f->name(), "startIndex")) startOff = f->offset();
        if (endOff < 0 && raw_is(f->name(), "endIndex")) endOff = f->offset();
        if (offsetOff < 0 && raw_is(f->name(), "offset")) offsetOff = f->offset();
      }
      if (!transientBit && plan->shape != SH_STRING && plan->shape != SH_CLASS
          && plan->shape != SH_ENUM && plan->shape != SH_ARRAY) {
        if (!ownerReady) {
          owner = dup_field_text(t, k->name(), &ownerN, 1);
          ownerReady = 1;
        }
        FieldPlan fp;
        memset(&fp, 0, sizeof fp);
        fp.owner = dup_field_text(t, k->name(), &fp.ownerN, 1);
        fp.name = dup_field_text(t, f->name(), &fp.nameN, 0);
        fp.spec = dup_field_text(t, f->spec(), &fp.specN, 0);
        fp.offset = f->offset();
        fp.code = f->code();
        add_pojo_field(t, plan, fp);
      }
    }
    free(owner);
  }

  plan->innerOff = innerOff;
  plan->elementOff = elementOff;
  plan->mapOff = mapOff;
  plan->frontOff = frontOff;
  plan->sizeOff = sizeOff;
  plan->arrayOff = namedOff >= 0 ? namedOff : anyArray;
  plan->contentOff = contentOff;
  plan->listOff = listOff;
  plan->collectionOff = collectionOff;
  plan->parentOff = parentOff;
  plan->setOff = setOff;
  plan->dataOff = dataOff;
  plan->startOff = startOff;
  plan->endOff = endOff;
  plan->offsetOff = offsetOff;
  plan->soleOff = soleOff;

  if (plan->shape != SH_POJO) {
    return plan;
  }

  int isMap = implements_(class_, mapC, 0);
  int isSet = implements_(class_, setC, 0);
  int isCol = implements_(class_, colC, 0);
  plan->tag = isMap ? T_MAP : isSet ? T_SET : T_LIST;
  int unmod = plan->name && strstr(plan->name, "Unmodifiable");
  int singleton = plan->name && strstr(plan->name, "Singleton");
  int chm = name_ends(plan->name, "ConcurrentHashMap");
  int cowal = name_ends(plan->name, "CopyOnWriteArrayList");
  int treeMap = name_ends(plan->name, "TreeMap");
  int treeSet = name_ends(plan->name, "TreeSet");
  int sub = name_ends(plan->name, "SubList");
  if (unmod && innerOff >= 0) {
    plan->shape = SH_WRAPPER;
  } else if ((isSet && refCount == 1 && elementOff >= 0)
             || (singleton && elementOff >= 0)) {
    plan->shape = SH_SINGLETON;
    if (!isSet && !isMap) {
      plan->tag = T_LIST;
    }
  } else if (isSet && mapOff >= 0) {
    plan->shape = SH_HASHSET;
  } else if (frontOff >= 0 && sizeOff >= 0) {
    plan->shape = SH_LINKED;
  } else if (isMap && sizeOff >= 0 && anyArray >= 0) {
    plan->shape = SH_HASHMAP;
    plan->arrayOff = anyArray;
  } else if (namedOff >= 0 && sizeOff >= 0) {
    plan->shape = SH_ARRAYLIST;
    plan->arrayOff = namedOff;
  } else if ((isCol || isMap || isSet) && dataOff >= 0 && startOff >= 0 && endOff >= 0
             && sizeOff >= 0) {
    plan->shape = SH_DEQUE;
  } else if (isMap && chm && contentOff >= 0) {
    plan->shape = SH_CHM;
  } else if (cowal && namedOff >= 0) {
    plan->shape = SH_PLAIN;
    plan->arrayOff = namedOff;
  } else if (isMap && treeMap && setOff >= 0) {
    plan->shape = SH_DELEGATE;
    plan->delegateOff = setOff;
  } else if (isSet && treeSet && setOff >= 0) {
    plan->shape = SH_TREE;
  } else if (sub && parentOff >= 0 && offsetOff >= 0 && sizeOff >= 0) {
    plan->shape = SH_SUBLIST;
  } else if (isMap && mapOff >= 0) {
    plan->shape = SH_DELEGATE;
    plan->delegateOff = mapOff;
  } else if ((isCol || isSet) && collectionOff >= 0) {
    plan->shape = SH_DELEGATE;
    plan->delegateOff = collectionOff;
  } else if ((isCol || isSet) && listOff >= 0) {
    plan->shape = SH_DELEGATE;
    plan->delegateOff = listOff;
  } else if (refArrayCount == 1 && refCount == 1 && sizeOff < 0) {
    plan->shape = SH_PLAIN;
    plan->arrayOff = anyArray;
  } else if ((isCol || isMap || isSet) && refCount == 0) {
    plan->shape = SH_EMPTY;
  } else if ((isCol || isMap || isSet) && soleOff >= 0) {
    plan->shape = SH_DELEGATE;
    plan->delegateOff = soleOff;
  } else if (isCol || isMap || isSet) {
    plan->shape = SH_UNKNOWN;
  } else {
    plan->shape = SH_POJO;
    plan->tag = T_OBJECT;
  }
  return plan;
}

GcClass* resolve_internal(Thread* t, const char* internal, int doThrow)
{
  GcByteArray* name = makeByteArray(t, "%s", internal);
  PROTECT(t, name);
  GcClass* c = resolveSystemClass(t, roots(t)->bootLoader(), name, false);
  if (c) {
    return c;
  }
  if (t->exception) {
    t->exception = 0;
  }
  GcClassLoader* app = roots(t)->appLoader();
  if (app) {
    c = resolveSystemClass(t, app, name, doThrow);
  }
  if (!c && doThrow && !t->exception) {
    fail(t, 0, "bad state payload");
  }
  return c;
}

GcClass* boot_named(Thread* t, const char* name, GcClass** slot)
{
  GcClass* found = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (found) {
    return found;
  }
  found = resolve_internal(t, name, 1);
  __atomic_store_n(slot, found, __ATOMIC_RELEASE);
  return found;
}

void load_collection_classes(Thread* t, GcClass** mapC, GcClass** colC, GcClass** setC)
{
  if (g_walk) {
    *mapC = g_walk->mapC;
    *colC = g_walk->colC;
    *setC = g_walk->setC;
    return;
  }
  if (g_hot) {
    *mapC = g_mapC;
    *colC = g_colC;
    *setC = g_setC;
    return;
  }
  GcClass* map = resolve_internal(t, "java/util/Map", 1);
  PROTECT(t, map);
  GcClass* col = resolve_internal(t, "java/util/Collection", 1);
  PROTECT(t, col);
  GcClass* set = resolve_internal(t, "java/util/Set", 1);
  *mapC = map;
  *colC = col;
  *setC = set;
}

uint8_t* alloc_stub(Thread* t)
{
  if (pthread_mutex_lock(&g_stubMu) != 0) {
    fail(t, 1, "OutOfMemoryError");
  }
  if (!g_slab || g_slabUsed + kStubBytes > kSlabBytes) {
    void* p = mmap(0,
                   kSlabBytes,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
    if (p == MAP_FAILED) {
      pthread_mutex_unlock(&g_stubMu);
      fail(t, 1, "OutOfMemoryError");
    }
    g_slab = static_cast<uint8_t*>(p);
    g_slabUsed = 0;
  }
  uint8_t* s = g_slab + g_slabUsed;
  g_slabUsed += kStubBytes;
  pthread_mutex_unlock(&g_stubMu);
  memset(s, 0, kStubBytes);
  return s;
}

void write_ser_stub(uint8_t* s, Plan* plan)
{
  s[0] = 0x48;
  s[1] = 0xBA;
  memcpy(s + 2, &plan, 8);
  s[10] = 0x49;
  s[11] = 0xBB;
  uintptr_t fn = reinterpret_cast<uintptr_t>(&entry);
  memcpy(s + 12, &fn, 8);
  s[20] = 0x41;
  s[21] = 0xFF;
  s[22] = 0xE3;
}

void write_de_stub(uint8_t* s, Plan* plan)
{
  s[0] = 0x48;
  s[1] = 0xBE;
  memcpy(s + 2, &plan, 8);
  s[10] = 0x49;
  s[11] = 0xBB;
  uintptr_t fn = reinterpret_cast<uintptr_t>(&dentry);
  memcpy(s + 12, &fn, 8);
  s[20] = 0x41;
  s[21] = 0xFF;
  s[22] = 0xE3;
}

void ensure_fn(Thread* t, GcClass* class_)
{
  PROTECT(t, class_);
  if (__atomic_load_n(&class_->serializeThunk(), __ATOMIC_ACQUIRE) != 0) {
    while (__atomic_load_n(&class_->deserializeThunk(), __ATOMIC_ACQUIRE) == 0) {
      cpu_pause();
    }
    return;
  }
  GcClass* mapC = 0;
  GcClass* colC = 0;
  GcClass* setC = 0;
  load_collection_classes(t, &mapC, &colC, &setC);
  Plan* plan = build_plan(t, class_, mapC, colC, setC);
  if (!plan) {
    fail(t, 1, "OutOfMemoryError");
  }
  uint8_t* ss = alloc_stub(t);
  uint8_t* ds = alloc_stub(t);
  write_ser_stub(ss, plan);
  write_de_stub(ds, plan);
  __builtin___clear_cache(reinterpret_cast<char*>(ss),
                          reinterpret_cast<char*>(ss) + kStubBytes);
  __builtin___clear_cache(reinterpret_cast<char*>(ds),
                          reinterpret_cast<char*>(ds) + kStubBytes);
  uint64_t expected = 0;
  if (!__atomic_compare_exchange_n(&class_->serializeThunk(),
                                   &expected,
                                   reinterpret_cast<uint64_t>(ss),
                                   false,
                                   __ATOMIC_RELEASE,
                                   __ATOMIC_RELAXED)) {
    free_plan(plan);
    while (__atomic_load_n(&class_->deserializeThunk(), __ATOMIC_ACQUIRE) == 0) {
      cpu_pause();
    }
    return;
  }
  __atomic_store_n(
      &class_->deserializeThunk(), reinterpret_cast<uint64_t>(ds), __ATOMIC_RELEASE);
}

typedef GcByteArray* (*SerFn)(Thread*, object);

void call_ser(Walk* w, object obj)
{
  GcClass* c = objectClass(w->t, obj);
  if (__atomic_load_n(&c->serializeThunk(), __ATOMIC_ACQUIRE) == 0) {
    ensure_fn(w->t, c);
  }
  SerFn fn = reinterpret_cast<SerFn>(static_cast<uintptr_t>(
      __atomic_load_n(&c->serializeThunk(), __ATOMIC_ACQUIRE)));
  if (!fn) {
    fail(w->t, 0, "bad state payload");
  }
  fn(w->t, obj);
}

void write_value(Walk* w, object obj)
{
  ++w->depth;
  if (w->depth > static_cast<int>(kMaxDepth)) {
    fail(w->t, 0, "state graph is too deep");
  }
  if (!obj) {
    put_u8(w, T_NULL);
    --w->depth;
    return;
  }
  int existing = id_find(w->ids, obj);
  if (existing >= 0) {
    put_u8(w, T_BACKREF);
    put_u32(w, static_cast<uint32_t>(existing));
    --w->depth;
    return;
  }
  call_ser(w, obj);
  --w->depth;
}

void preview(Walk* w, object obj, uint8_t** data, uint32_t* len)
{
  Buf buf;
  memset(&buf, 0, sizeof buf);
  buf.next = w->bufs;
  w->bufs = &buf;
  IdTab child;
  memset(&child, 0, sizeof child);
  child.parent = w->ids;
  child.next = w->idList;
  w->idList = &child;
  Buf* oldOut = w->out;
  IdTab* oldIds = w->ids;
  w->out = &buf;
  w->ids = &child;
  write_value(w, obj);
  w->out = oldOut;
  w->ids = oldIds;
  w->bufs = buf.next;
  w->idList = child.next;
  free(child.slots);
  child.slots = 0;
  *data = buf.data;
  *len = buf.len;
  buf.data = 0;
}

int piece_cmp(const void* a, const void* b)
{
  const Piece* pa = *static_cast<const Piece* const*>(a);
  const Piece* pb = *static_cast<const Piece* const*>(b);
  uint32_t n = pa->keyN < pb->keyN ? pa->keyN : pb->keyN;
  int c = 0;
  if (n) {
    c = memcmp(pa->key, pb->key, n);
  }
  if (c) {
    return c;
  }
  if (pa->keyN != pb->keyN) {
    return pa->keyN < pb->keyN ? -1 : 1;
  }
  n = pa->valN < pb->valN ? pa->valN : pb->valN;
  if (n && pa->val && pb->val) {
    c = memcmp(pa->val, pb->val, n);
  }
  if (c) {
    return c;
  }
  if (pa->valN != pb->valN) {
    return pa->valN < pb->valN ? -1 : 1;
  }
  return 0;
}

void write_int_tagged(Walk* w, int32_t value)
{
  put_u8(w, T_INT);
  put_u32(w, static_cast<uint32_t>(value));
}

void write_prim_value(Walk* w, int code, uint64_t bits)
{
  switch (code) {
  case BooleanField:
    put_u8(w, T_BOOL);
    put_u8(w, bits ? 1 : 0);
    break;
  case ByteField:
    write_int_tagged(w, static_cast<int32_t>(static_cast<int8_t>(bits)));
    break;
  case CharField:
    write_int_tagged(w, static_cast<int32_t>(static_cast<uint16_t>(bits)));
    break;
  case ShortField:
    write_int_tagged(w, static_cast<int32_t>(static_cast<int16_t>(bits)));
    break;
  case IntField:
    write_int_tagged(w, static_cast<int32_t>(bits));
    break;
  case LongField:
    put_u8(w, T_LONG);
    put_u64(w, bits);
    break;
  case FloatField:
    put_u8(w, T_FLOAT);
    put_u32(w, static_cast<uint32_t>(bits));
    break;
  case DoubleField:
    put_u8(w, T_DOUBLE);
    put_u64(w, bits);
    break;
  default:
    fail(w->t, 0, "bad state payload");
  }
}

uint64_t read_prim_at(object obj, int code, int offset)
{
  unsigned off = static_cast<unsigned>(offset);
  switch (code) {
  case BooleanField:
    return fieldAtOffset<uint8_t>(obj, off) ? 1 : 0;
  case ByteField:
    return static_cast<uint32_t>(fieldAtOffset<int8_t>(obj, off));
  case CharField:
    return fieldAtOffset<uint16_t>(obj, off);
  case ShortField:
    return static_cast<uint32_t>(fieldAtOffset<int16_t>(obj, off));
  case IntField:
    return static_cast<uint32_t>(fieldAtOffset<int32_t>(obj, off));
  case LongField:
    return static_cast<uint64_t>(fieldAtOffset<int64_t>(obj, off));
  case FloatField:
    return fieldAtOffset<uint32_t>(obj, off);
  case DoubleField:
    return fieldAtOffset<uint64_t>(obj, off);
  default:
    return 0;
  }
}

uint64_t read_array_prim(object array, int code, uint32_t index)
{
  switch (code) {
  case BooleanField:
    return fieldAtOffset<uint8_t>(array, ArrayBody + index) ? 1 : 0;
  case ByteField:
    return static_cast<uint32_t>(
        fieldAtOffset<int8_t>(array, ArrayBody + index));
  case CharField:
    return fieldAtOffset<uint16_t>(array, ArrayBody + index * 2);
  case ShortField:
    return static_cast<uint32_t>(
        fieldAtOffset<int16_t>(array, ArrayBody + index * 2));
  case IntField:
    return static_cast<uint32_t>(
        fieldAtOffset<int32_t>(array, ArrayBody + index * 4));
  case FloatField:
    return fieldAtOffset<uint32_t>(array, ArrayBody + index * 4);
  case LongField:
    return static_cast<uint64_t>(
        fieldAtOffset<int64_t>(array, ArrayBody + index * 8));
  case DoubleField:
    return fieldAtOffset<uint64_t>(array, ArrayBody + index * 8);
  default:
    return 0;
  }
}

void write_string(Walk* w, object self)
{
  id_add(w, self);
  put_u8(w, T_STRING);
  const uint8_t* p;
  unsigned n = 0;
  string_bytes(self, p, n);
  put_mutf8_utf8(w, p, n);
}

void write_class(Walk* w, object self, Plan* plan)
{
  id_add(w, self);
  put_u8(w, T_CLASS);
  object vm = fieldAtOffset<object>(self, static_cast<unsigned>(plan->vmOff));
  put_dotted_class(w, reinterpret_cast<GcClass*>(vm));
}

void write_enum(Walk* w, object self, Plan* plan)
{
  id_add(w, self);
  put_u8(w, T_ENUM);
  put_plan_str(w, plan->enumName, plan->enumNameN);
  if (plan->enumNameOff < 0) {
    fail(w->t, 0, "bad state payload");
  }
  object name = fieldAtOffset<object>(self, static_cast<unsigned>(plan->enumNameOff));
  const uint8_t* p;
  unsigned n = 0;
  if (name) {
    string_bytes(name, p, n);
  } else {
    p = reinterpret_cast<const uint8_t*>("");
  }
  put_mutf8_utf8(w, p, n);
}

void write_array(Walk* w, object self, Plan* plan)
{
  unsigned n = array_len(self);
  if (n > 0x7fffffffu) {
    fail(w->t, 0, "bad state payload");
  }
  id_add(w, self);
  put_u8(w, T_ARRAY);
  put_plan_str(w, plan->component, plan->componentN);
  put_u32(w, n);
  if (plan->prim == ObjectField) {
    for (unsigned i = 0; i < n; ++i) {
      write_value(w, array_at(self, i));
    }
    return;
  }
  for (unsigned i = 0; i < n; ++i) {
    write_prim_value(w, plan->prim, read_array_prim(self, plan->prim, i));
  }
}

void write_pojo(Walk* w, object self, Plan* plan)
{
  id_add(w, self);
  put_u8(w, T_OBJECT);
  put_plan_str(w, plan->name, plan->nameN);
  put_u32(w, static_cast<uint32_t>(plan->fieldCount));
  for (int i = 0; i < plan->fieldCount; ++i) {
    FieldPlan* f = &plan->fields[i];
    put_plan_str(w, f->owner, f->ownerN);
    put_plan_str(w, f->name, f->nameN);
    put_plan_str(w, f->spec, f->specN);
    if (f->code == ObjectField) {
      object value = fieldAtOffset<object>(self, static_cast<unsigned>(f->offset));
      write_value(w, value);
    } else {
      write_prim_value(w, f->code, read_prim_at(self, f->code, f->offset));
    }
  }
}

void cell_offs(Walk* w, GcClass* c, int* a, int* b, int* c3, const char* na, const char* nb, const char* nc)
{
  for (OffCache* it = w->cells; it; it = it->next) {
    if (it->cls == c) {
      *a = it->a;
      *b = it->b;
      *c3 = it->c;
      return;
    }
  }
  OffCache* node = static_cast<OffCache*>(xmalloc(w->t, sizeof(OffCache)));
  node->cls = c;
  node->a = find_field_off(c, na, ObjectField, 1);
  node->b = nb ? find_field_off(c, nb, ObjectField, 1) : -1;
  node->c = nc ? find_field_off(c, nc, ObjectField, 1) : -1;
  node->next = w->cells;
  w->cells = node;
  *a = node->a;
  *b = node->b;
  *c3 = node->c;
}

struct ObjBuf {
  object* p;
  uint32_t n;
  uint32_t cap;
};

struct Kv {
  object k;
  object v;
};

struct KvBuf {
  Kv* p;
  uint32_t n;
  uint32_t cap;
};

void obj_push(Walk* w, ObjBuf* b, object o)
{
  if (b->n == b->cap) {
    uint32_t cap = b->cap ? b->cap * 2 : 8;
    b->p = static_cast<object*>(
        xrealloc(w->t, b->p, static_cast<size_t>(cap) * sizeof(object)));
    b->cap = cap;
  }
  b->p[b->n++] = o;
}

void kv_push(Walk* w, KvBuf* b, object k, object v)
{
  if (b->n == b->cap) {
    uint32_t cap = b->cap ? b->cap * 2 : 8;
    b->p = static_cast<Kv*>(
        xrealloc(w->t, b->p, static_cast<size_t>(cap) * sizeof(Kv)));
    b->cap = cap;
  }
  b->p[b->n].k = k;
  b->p[b->n].v = v;
  ++b->n;
}

Plan* plan_from_thunk(GcClass* c)
{
  uint64_t stub = __atomic_load_n(&c->serializeThunk(), __ATOMIC_ACQUIRE);
  const uint8_t* s = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(stub));
  Plan* plan = 0;
  memcpy(&plan, s + 2, 8);
  return plan;
}

void collect_shape(Walk* w, object obj, Plan* plan, ObjBuf* elems, KvBuf* pairs, int mode, int depth);
void collect_object(Walk* w, object obj, ObjBuf* elems, KvBuf* pairs, int mode, int depth);

void emit_mapped(Walk* w, object key, object val, object cell, ObjBuf* elems, KvBuf* pairs, int mode)
{
  if (mode == MODE_PAIRS) {
    kv_push(w, pairs, key, val);
  } else if (mode == MODE_VALUES) {
    obj_push(w, elems, val);
  } else if (mode == MODE_CELLS) {
    obj_push(w, elems, cell);
  } else {
    obj_push(w, elems, key);
  }
}

void hash_cells(Walk* w, object mapObj, int arrayOff, ObjBuf* keys, KvBuf* pairs, int mode)
{
  if (arrayOff < 0) {
    fail(w->t, 0, "cannot serialize collection");
  }
  object array = fieldAtOffset<object>(mapObj, static_cast<unsigned>(arrayOff));
  if (!array) {
    return;
  }
  unsigned n = array_len(array);
  unsigned hops = 0;
  for (unsigned i = 0; i < n; ++i) {
    object cell = array_at(array, i);
    while (cell) {
      if (++hops > kHopCap) {
        fail(w->t, 0, "cannot serialize collection");
      }
      int keyOff = -1;
      int valOff = -1;
      int nextOff = -1;
      cell_offs(w, objectClass(w->t, cell), &keyOff, &valOff, &nextOff, "key", "value", "next");
      if (keyOff < 0 || nextOff < 0) {
        fail(w->t, 0, "cannot serialize collection");
      }
      object key = fieldAtOffset<object>(cell, static_cast<unsigned>(keyOff));
      object val = valOff < 0
                       ? 0
                       : fieldAtOffset<object>(cell, static_cast<unsigned>(valOff));
      object next = fieldAtOffset<object>(cell, static_cast<unsigned>(nextOff));
      emit_mapped(w, key, val, cell, keys, pairs, mode);
      cell = next;
    }
  }
}

typedef void (*NodeVisit)(Walk*, object, ObjBuf*, KvBuf*, int, unsigned*);

void walk_tree(Walk* w, object node, NodeVisit visit, ObjBuf* elems, KvBuf* pairs, int mode, unsigned* hops)
{
  if (!node) {
    return;
  }
  if (++*hops > kHopCap) {
    fail(w->t, 0, "cannot serialize collection");
  }
  int valueOff = -1;
  int leftOff = -1;
  int rightOff = -1;
  cell_offs(w, objectClass(w->t, node), &valueOff, &leftOff, &rightOff, "value", "left", "right");
  if (valueOff < 0 || leftOff < 0 || rightOff < 0) {
    fail(w->t, 0, "cannot serialize collection");
  }
  object left = fieldAtOffset<object>(node, static_cast<unsigned>(leftOff));
  if (left == node) {
    return;
  }
  walk_tree(w, left, visit, elems, pairs, mode, hops);
  visit(w, fieldAtOffset<object>(node, static_cast<unsigned>(valueOff)), elems, pairs, mode, hops);
  object right = fieldAtOffset<object>(node, static_cast<unsigned>(rightOff));
  if (right != node) {
    walk_tree(w, right, visit, elems, pairs, mode, hops);
  }
}

void visit_chm(Walk* w, object chmNode, ObjBuf* elems, KvBuf* pairs, int mode, unsigned* hops)
{
  if (!chmNode) {
    return;
  }
  int cellOff = find_field_off(objectClass(w->t, chmNode), "value", ObjectField, 1);
  if (cellOff < 0) {
    fail(w->t, 0, "cannot serialize collection");
  }
  object cell = fieldAtOffset<object>(chmNode, static_cast<unsigned>(cellOff));
  while (cell) {
    if (++*hops > kHopCap) {
      fail(w->t, 0, "cannot serialize collection");
    }
    int keyOff = -1;
    int valOff = -1;
    int nextOff = -1;
    cell_offs(w, objectClass(w->t, cell), &keyOff, &valOff, &nextOff, "key", "value", "next");
    if (keyOff < 0 || nextOff < 0) {
      fail(w->t, 0, "cannot serialize collection");
    }
    object key = fieldAtOffset<object>(cell, static_cast<unsigned>(keyOff));
    object val = valOff < 0 ? 0 : fieldAtOffset<object>(cell, static_cast<unsigned>(valOff));
    object next = fieldAtOffset<object>(cell, static_cast<unsigned>(nextOff));
    emit_mapped(w, key, val, cell, elems, pairs, mode);
    cell = next;
  }
}

void visit_tree(Walk* w, object cell, ObjBuf* elems, KvBuf* pairs, int mode, unsigned* hops)
{
  while (cell) {
    if (++*hops > kHopCap) {
      fail(w->t, 0, "cannot serialize collection");
    }
    int valueOff = -1;
    int nextOff = -1;
    int ignored = -1;
    cell_offs(w, objectClass(w->t, cell), &valueOff, &nextOff, &ignored, "value", "next", 0);
    object elem = valueOff < 0 ? cell : fieldAtOffset<object>(cell, static_cast<unsigned>(valueOff));
    if (mode == MODE_PAIRS || mode == MODE_VALUES) {
      int keyOff = elem ? find_field_off(objectClass(w->t, elem), "key", ObjectField, 1) : -1;
      int valOff = elem ? find_field_off(objectClass(w->t, elem), "value", ObjectField, 1) : -1;
      if (mode == MODE_PAIRS) {
        if (keyOff < 0) {
          fail(w->t, 0, "cannot serialize collection");
        }
        object key = fieldAtOffset<object>(elem, static_cast<unsigned>(keyOff));
        object val = valOff < 0 ? 0 : fieldAtOffset<object>(elem, static_cast<unsigned>(valOff));
        kv_push(w, pairs, key, val);
      } else if (valOff >= 0) {
        obj_push(w, elems, fieldAtOffset<object>(elem, static_cast<unsigned>(valOff)));
      } else {
        obj_push(w, elems, elem);
      }
    } else if (mode == MODE_CELLS) {
      obj_push(w, elems, cell);
    } else {
      obj_push(w, elems, elem);
    }
    if (valueOff < 0 || nextOff < 0) {
      break;
    }
    cell = fieldAtOffset<object>(cell, static_cast<unsigned>(nextOff));
  }
}

void walk_pset(Walk* w, object pset, NodeVisit visit, ObjBuf* elems, KvBuf* pairs, int mode)
{
  if (!pset) {
    return;
  }
  int rootOff = find_field_off(objectClass(w->t, pset), "root", ObjectField, 1);
  if (rootOff < 0) {
    fail(w->t, 0, "cannot serialize collection");
  }
  unsigned hops = 0;
  walk_tree(w,
            fieldAtOffset<object>(pset, static_cast<unsigned>(rootOff)),
            visit,
            elems,
            pairs,
            mode,
            &hops);
}

void NO_RETURN fail_collection(Thread* t, const Plan* plan)
{
  char msg[512];
  snprintf(msg, sizeof msg, "cannot serialize collection %s", plan && plan->name ? plan->name : "?");
  fail(t, 0, msg);
}

void collect_shape(Walk* w, object obj, Plan* plan, ObjBuf* elems, KvBuf* pairs, int mode, int depth)
{
  if (!obj || !plan) {
    return;
  }
  if (depth > 8) {
    fail(w->t, 0, "cannot serialize collection");
  }
  object cur = obj;
  Plan* body = plan;
  int wraps = 0;
  while (body->shape == SH_WRAPPER && wraps < 8) {
    if (body->innerOff < 0) {
      return;
    }
    object inner = fieldAtOffset<object>(cur, static_cast<unsigned>(body->innerOff));
    if (!inner || inner == cur) {
      return;
    }
    GcClass* ic = objectClass(w->t, inner);
    if (__atomic_load_n(&ic->serializeThunk(), __ATOMIC_ACQUIRE) == 0) {
      ensure_fn(w->t, ic);
    }
    body = plan_from_thunk(ic);
    cur = inner;
    ++wraps;
  }
  if (body->shape == SH_EMPTY) {
    return;
  }
  if (body->shape == SH_POJO || body->shape == SH_UNKNOWN) {
    if (body->soleOff < 0) {
      fail_collection(w->t, body);
    }
    object next = fieldAtOffset<object>(cur, static_cast<unsigned>(body->soleOff));
    if (!next || next == cur) {
      return;
    }
    collect_object(w, next, elems, pairs, mode, depth + 1);
    return;
  }
  if (body->shape == SH_HASHMAP) {
    hash_cells(w, cur, body->arrayOff, elems, pairs, mode);
    return;
  }
  if (body->shape == SH_CHM) {
    if (body->contentOff < 0) {
      fail_collection(w->t, body);
    }
    object content = fieldAtOffset<object>(cur, static_cast<unsigned>(body->contentOff));
    if (!content) {
      return;
    }
    int setOff = find_field_off(objectClass(w->t, content), "set", ObjectField, 1);
    if (setOff < 0) {
      fail_collection(w->t, body);
    }
    walk_pset(w, fieldAtOffset<object>(content, static_cast<unsigned>(setOff)), visit_chm, elems, pairs, mode);
    return;
  }
  if (body->shape == SH_TREE) {
    if (body->setOff < 0) {
      fail_collection(w->t, body);
    }
    walk_pset(w,
              fieldAtOffset<object>(cur, static_cast<unsigned>(body->setOff)),
              visit_tree,
              elems,
              pairs,
              mode);
    return;
  }
  if (body->shape == SH_DELEGATE) {
    if (body->delegateOff < 0) {
      fail_collection(w->t, body);
    }
    object inner = fieldAtOffset<object>(cur, static_cast<unsigned>(body->delegateOff));
    if (!inner || inner == cur) {
      return;
    }
    collect_object(w, inner, elems, pairs, mode, depth + 1);
    return;
  }
  if (body->shape == SH_HASHSET) {
    if (body->mapOff < 0) {
      return;
    }
    object map = fieldAtOffset<object>(cur, static_cast<unsigned>(body->mapOff));
    if (!map || map == cur) {
      return;
    }
    collect_object(w, map, elems, pairs, mode, depth + 1);
    return;
  }
  if (mode == MODE_PAIRS) {
    fail_collection(w->t, body);
  }
  if (body->shape == SH_SINGLETON) {
    object elem = body->elementOff < 0
                      ? 0
                      : fieldAtOffset<object>(cur, static_cast<unsigned>(body->elementOff));
    obj_push(w, elems, elem);
    return;
  }
  if (body->shape == SH_LINKED) {
    if (body->frontOff < 0) {
      return;
    }
    object cell = fieldAtOffset<object>(cur, static_cast<unsigned>(body->frontOff));
    unsigned hops = 0;
    while (cell) {
      if (++hops > kHopCap) {
        fail(w->t, 0, "cannot serialize collection");
      }
      int valueOff = -1;
      int nextOff = -1;
      int ignored = -1;
      cell_offs(w, objectClass(w->t, cell), &valueOff, &nextOff, &ignored, "value", "next", 0);
      if (valueOff < 0 || nextOff < 0) {
        fail(w->t, 0, "cannot serialize collection");
      }
      obj_push(w, elems, fieldAtOffset<object>(cell, static_cast<unsigned>(valueOff)));
      cell = fieldAtOffset<object>(cell, static_cast<unsigned>(nextOff));
    }
    return;
  }
  if (body->shape == SH_DEQUE) {
    if (body->dataOff < 0 || body->startOff < 0 || body->sizeOff < 0) {
      fail_collection(w->t, body);
    }
    int32_t size = fieldAtOffset<int32_t>(cur, static_cast<unsigned>(body->sizeOff));
    int32_t start = fieldAtOffset<int32_t>(cur, static_cast<unsigned>(body->startOff));
    object array = fieldAtOffset<object>(cur, static_cast<unsigned>(body->dataOff));
    if (size <= 0) {
      return;
    }
    if (!array || start < 0) {
      fail_collection(w->t, body);
    }
    unsigned len = array_len(array);
    if (len == 0 || static_cast<unsigned>(size) > len) {
      fail_collection(w->t, body);
    }
    for (int32_t i = 0; i < size; ++i) {
      unsigned idx = (static_cast<unsigned>(start) + static_cast<unsigned>(i)) % len;
      obj_push(w, elems, array_at(array, idx));
    }
    return;
  }
  if (body->shape == SH_SUBLIST) {
    if (body->parentOff < 0 || body->offsetOff < 0 || body->sizeOff < 0) {
      fail_collection(w->t, body);
    }
    object parent = fieldAtOffset<object>(cur, static_cast<unsigned>(body->parentOff));
    ObjBuf all;
    memset(&all, 0, sizeof all);
    if (parent && parent != cur) {
      collect_object(w, parent, &all, 0, MODE_ELEMS, depth + 1);
    }
    int32_t off = fieldAtOffset<int32_t>(cur, static_cast<unsigned>(body->offsetOff));
    int32_t sz = fieldAtOffset<int32_t>(cur, static_cast<unsigned>(body->sizeOff));
    if (off < 0 || sz < 0
        || static_cast<uint64_t>(off) + static_cast<uint64_t>(sz) > all.n) {
      free(all.p);
      fail_collection(w->t, body);
    }
    for (int32_t i = 0; i < sz; ++i) {
      obj_push(w, elems, all.p[static_cast<uint32_t>(off + i)]);
    }
    free(all.p);
    return;
  }
  if (body->shape == SH_ARRAYLIST || body->shape == SH_PLAIN) {
    object array = body->arrayOff < 0
                       ? 0
                       : fieldAtOffset<object>(cur, static_cast<unsigned>(body->arrayOff));
    uint32_t n = 0;
    if (body->shape == SH_ARRAYLIST) {
      if (body->sizeOff < 0) {
        fail(w->t, 0, "cannot serialize collection");
      }
      int32_t size = fieldAtOffset<int32_t>(cur, static_cast<unsigned>(body->sizeOff));
      if (size < 0 || (size > 0 && !array) || (array && static_cast<uint32_t>(size) > array_len(array))) {
        fail(w->t, 0, "cannot serialize collection");
      }
      n = static_cast<uint32_t>(size);
    } else if (array) {
      n = array_len(array);
    }
    for (uint32_t i = 0; i < n; ++i) {
      obj_push(w, elems, array_at(array, i));
    }
    return;
  }
  fail_collection(w->t, body);
}

void collect_object(Walk* w, object obj, ObjBuf* elems, KvBuf* pairs, int mode, int depth)
{
  if (!obj) {
    return;
  }
  if (depth > 8) {
    fail(w->t, 0, "cannot serialize collection");
  }
  GcClass* c = objectClass(w->t, obj);
  if (__atomic_load_n(&c->serializeThunk(), __ATOMIC_ACQUIRE) == 0) {
    ensure_fn(w->t, c);
  }
  collect_shape(w, obj, plan_from_thunk(c), elems, pairs, mode, depth);
}

void write_sorted_objs(Walk* w, object* objs, uint32_t n)
{
  if (n > 0x7fffffffu) {
    fail(w->t, 0, "bad state payload");
  }
  Piece* mark = w->pieces;
  Piece** arr = static_cast<Piece**>(
      xmalloc(w->t, (n ? n : 1) * sizeof(Piece*)));
  Scratch sc;
  sc.p = arr;
  sc.next = w->scratches;
  w->scratches = &sc;
  for (uint32_t i = 0; i < n; ++i) {
    Piece* pc = static_cast<Piece*>(xmalloc(w->t, sizeof(Piece)));
    memset(pc, 0, sizeof(Piece));
    preview(w, objs[i], &pc->key, &pc->keyN);
    pc->keyObj = objs[i];
    pc->next = w->pieces;
    w->pieces = pc;
    arr[i] = pc;
  }
  if (n > 1) {
    qsort(arr, n, sizeof(Piece*), piece_cmp);
  }
  put_u32(w, n);
  for (uint32_t i = 0; i < n; ++i) {
    write_value(w, arr[i]->keyObj);
  }
  w->scratches = sc.next;
  free(arr);
  while (w->pieces != mark) {
    Piece* p = w->pieces;
    w->pieces = p->next;
    free(p->key);
    free(p->val);
    free(p);
  }
}

void write_sorted_pairs(Walk* w, Kv* rows, uint32_t n)
{
  Piece* mark = w->pieces;
  Piece** arr = static_cast<Piece**>(
      xmalloc(w->t, (n ? n : 1) * sizeof(Piece*)));
  Scratch sc;
  sc.p = arr;
  sc.next = w->scratches;
  w->scratches = &sc;
  for (uint32_t i = 0; i < n; ++i) {
    Piece* pc = static_cast<Piece*>(xmalloc(w->t, sizeof(Piece)));
    memset(pc, 0, sizeof(Piece));
    preview(w, rows[i].k, &pc->key, &pc->keyN);
    preview(w, rows[i].v, &pc->val, &pc->valN);
    pc->keyObj = rows[i].k;
    pc->valObj = rows[i].v;
    pc->next = w->pieces;
    w->pieces = pc;
    arr[i] = pc;
  }
  if (n > 1) {
    qsort(arr, n, sizeof(Piece*), piece_cmp);
  }
  put_u32(w, n);
  for (uint32_t i = 0; i < n; ++i) {
    write_value(w, arr[i]->keyObj);
    write_value(w, arr[i]->valObj);
  }
  w->scratches = sc.next;
  free(arr);
  while (w->pieces != mark) {
    Piece* p = w->pieces;
    w->pieces = p->next;
    free(p->key);
    free(p->val);
    free(p);
  }
}

void write_coll(Walk* w, object self, Plan* plan)
{
  if (plan->shape == SH_UNKNOWN) {
    char msg[512];
    snprintf(msg,
             sizeof msg,
             "cannot serialize collection %s",
             plan->name ? plan->name : "?");
    fail(w->t, 0, msg);
  }
  id_add(w, self);
  put_u8(w, plan->tag);
  put_plan_str(w, plan->name, plan->nameN);
  if (plan->tag == T_MAP) {
    KvBuf rows;
    memset(&rows, 0, sizeof rows);
    Scratch sc;
    sc.p = 0;
    sc.next = w->scratches;
    w->scratches = &sc;
    collect_shape(w, self, plan, 0, &rows, collect_mode(plan, 1), 0);
    sc.p = rows.p;
    write_sorted_pairs(w, rows.p, rows.n);
    w->scratches = sc.next;
    free(rows.p);
    return;
  }
  ObjBuf elems;
  memset(&elems, 0, sizeof elems);
  Scratch sc;
  sc.p = 0;
  sc.next = w->scratches;
  w->scratches = &sc;
  collect_shape(w, self, plan, &elems, 0, collect_mode(plan, 0), 0);
  sc.p = elems.p;
  if (plan->tag == T_SET) {
    write_sorted_objs(w, elems.p, elems.n);
  } else {
    put_u32(w, elems.n);
    for (uint32_t i = 0; i < elems.n; ++i) {
      write_value(w, elems.p[i]);
    }
  }
  w->scratches = sc.next;
  free(elems.p);
}

void write_planned(Walk* w, object self, Plan* plan)
{
  switch (plan->shape) {
  case SH_STRING:
    write_string(w, self);
    break;
  case SH_CLASS:
    write_class(w, self, plan);
    break;
  case SH_ENUM:
    write_enum(w, self, plan);
    break;
  case SH_ARRAY:
    write_array(w, self, plan);
    break;
  case SH_POJO:
    write_pojo(w, self, plan);
    break;
  default:
    write_coll(w, self, plan);
    break;
  }
}

GcByteArray* entry(Thread* t, object self, Plan* plan)
{
  if (g_walk) {
    write_planned(g_walk, self, plan);
    return 0;
  }
  Walk w;
  memset(&w, 0, sizeof w);
  w.t = t;
  w.mapC = g_mapC;
  w.colC = g_colC;
  w.setC = g_setC;
  g_mapC = 0;
  g_colC = 0;
  g_setC = 0;
  g_hot = 0;
  Buf root;
  memset(&root, 0, sizeof root);
  IdTab ids;
  memset(&ids, 0, sizeof ids);
  w.out = &root;
  w.ids = &ids;
  w.bufs = &root;
  w.idList = &ids;
  g_walk = &w;
  w.depth = 1;
  put_u8(&w, 1);
  write_planned(&w, self, plan);
  uint8_t* data = root.data;
  uint32_t len = root.len;
  root.data = 0;
  g_walk = 0;
  w.mapC = 0;
  w.colC = 0;
  w.setC = 0;
  release_walk(&w);
  if (len > 0x7fffffffu) {
    free(data);
    fail(t, 0, "bad state payload");
  }
  if (g_keep) {
    g_kept = data;
    g_kept_n = len;
    return 0;
  }
  GcByteArray* arr = makeByteArray(t, len);
  if (len && data) {
    memcpy(arr->body().begin(), data, len);
  }
  free(data);
  return arr;
}

uint32_t read_u32(Dec* d)
{
  if (static_cast<uint32_t>(d->end - d->p) < 4) {
    fail(d->t, 0, "truncated state");
  }
  uint32_t v = (static_cast<uint32_t>(d->p[0]) << 24)
               | (static_cast<uint32_t>(d->p[1]) << 16)
               | (static_cast<uint32_t>(d->p[2]) << 8)
               | static_cast<uint32_t>(d->p[3]);
  d->p += 4;
  return v;
}

uint64_t read_u64(Dec* d)
{
  if (static_cast<uint32_t>(d->end - d->p) < 8) {
    fail(d->t, 0, "truncated state");
  }
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | d->p[i];
  }
  d->p += 8;
  return v;
}

uint8_t read_u8(Dec* d)
{
  if (d->p >= d->end) {
    fail(d->t, 0, "truncated state");
  }
  return *d->p++;
}

Slice read_slice(Dec* d)
{
  uint32_t n = read_u32(d);
  if (n > static_cast<uint32_t>(d->end - d->p)) {
    fail(d->t, 0, "truncated state");
  }
  Slice s;
  s.p = d->p;
  s.n = n;
  d->p += n;
  return s;
}

void id_grow_dec(Dec* d)
{
  uint32_t cap = d->idCap ? d->idCap * 2 : 16;
  object neu = makeObjectArray(d->t, cap);
  PROTECT(d->t, neu);
  GcArray* src = reinterpret_cast<GcArray*>(d->ids);
  GcArray* dst = reinterpret_cast<GcArray*>(neu);
  for (uint32_t i = 0; i < d->idCount; ++i) {
    dst->setBodyElement(d->t, i, src->body()[i]);
  }
  d->ids = neu;
  d->idCap = cap;
}

void add_id_dec(Dec* d, object obj)
{
  PROTECT(d->t, obj);
  if (d->idCount == d->idCap) {
    id_grow_dec(d);
  }
  reinterpret_cast<GcArray*>(d->ids)->setBodyElement(d->t, d->idCount, obj);
  ++d->idCount;
}

object id_at(Dec* d, uint32_t id)
{
  if (id >= d->idCount) {
    fail(d->t, 0, "bad backref");
  }
  return reinterpret_cast<GcArray*>(d->ids)->body()[id];
}

void copy_internal(Dec* d, Slice name, char* buf, unsigned cap)
{
  if (name.n >= cap) {
    fail(d->t, 0, "bad state payload");
  }
  for (uint32_t i = 0; i < name.n; ++i) {
    char ch = static_cast<char>(name.p[i]);
    buf[i] = ch == '.' ? '/' : ch;
  }
  buf[name.n] = 0;
}

GcClass* resolve_dotted(Dec* d, Slice name, int doThrow)
{
  char buf[1024];
  copy_internal(d, name, buf, sizeof buf);
  const char* prims = 0;
  (void)prims;
  if (name.n == 7 && memcmp(name.p, "boolean", 7) == 0)
    return raw_type(d->t, GcJboolean::Type);
  if (name.n == 4 && memcmp(name.p, "byte", 4) == 0)
    return raw_type(d->t, GcJbyte::Type);
  if (name.n == 4 && memcmp(name.p, "char", 4) == 0)
    return raw_type(d->t, GcJchar::Type);
  if (name.n == 5 && memcmp(name.p, "short", 5) == 0)
    return raw_type(d->t, GcJshort::Type);
  if (name.n == 3 && memcmp(name.p, "int", 3) == 0)
    return raw_type(d->t, GcJint::Type);
  if (name.n == 4 && memcmp(name.p, "long", 4) == 0)
    return raw_type(d->t, GcJlong::Type);
  if (name.n == 5 && memcmp(name.p, "float", 5) == 0)
    return raw_type(d->t, GcJfloat::Type);
  if (name.n == 6 && memcmp(name.p, "double", 6) == 0)
    return raw_type(d->t, GcJdouble::Type);
  if (name.n == 4 && memcmp(name.p, "void", 4) == 0)
    return raw_type(d->t, GcJvoid::Type);
  return resolve_internal(d->t, buf, doThrow);
}

bool slice_has(Slice name, const char* lit)
{
  unsigned m = static_cast<unsigned>(strlen(lit));
  if (name.n < m) {
    return false;
  }
  for (uint32_t i = 0; i + m <= name.n; ++i) {
    if (memcmp(name.p + i, lit, m) == 0) {
      return true;
    }
  }
  return false;
}

GcMethod* cache_method(Dec* d, int slot, const char* cls, const char* name, const char* spec)
{
  GcArray* cache = reinterpret_cast<GcArray*>(d->cache);
  object cur = cache->body()[slot];
  if (cur) {
    return reinterpret_cast<GcMethod*>(cur);
  }
  GcClass* c = resolve_internal(d->t, cls, 1);
  PROTECT(d->t, c);
  GcMethod* m = findMethodOrNull(d->t, c, name, spec);
  if (!m) {
    fail(d->t, 0, "bad state payload");
  }
  cache->setBodyElement(d->t, slot, m);
  return m;
}

object box_bool(Dec* d, uint32_t bit)
{
  GcMethod* m = cache_method(
      d, K_BOOL, "java/lang/Boolean", "valueOf", "(Z)Ljava/lang/Boolean;");
  object r = d->t->m->processor->invoke(d->t, m, 0, bit ? static_cast<uint32_t>(1) : static_cast<uint32_t>(0));
  check_exc(d->t);
  return r;
}

object box_int(Dec* d, int32_t value)
{
  GcMethod* m = cache_method(
      d, K_INT, "java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;");
  object r = d->t->m->processor->invoke(
      d->t, m, 0, static_cast<uint32_t>(value));
  check_exc(d->t);
  return r;
}

object box_long(Dec* d, uint64_t bits)
{
  GcMethod* m = cache_method(
      d, K_LONG, "java/lang/Long", "valueOf", "(J)Ljava/lang/Long;");
  object r = d->t->m->processor->invoke(d->t, m, 0, bits);
  check_exc(d->t);
  return r;
}

object box_float(Dec* d, uint32_t bits)
{
  GcMethod* m = cache_method(
      d, K_FLOAT, "java/lang/Float", "valueOf", "(F)Ljava/lang/Float;");
  float f;
  memcpy(&f, &bits, 4);
  double widened = f;
  object r = d->t->m->processor->invoke(d->t, m, 0, widened);
  check_exc(d->t);
  return r;
}

object box_double(Dec* d, uint64_t bits)
{
  GcMethod* m = cache_method(
      d, K_DOUBLE, "java/lang/Double", "valueOf", "(D)Ljava/lang/Double;");
  double v;
  memcpy(&v, &bits, 8);
  object r = d->t->m->processor->invoke(d->t, m, 0, v);
  check_exc(d->t);
  return r;
}

int64_t unbox_value(object obj, int* ok)
{
  *ok = 0;
  if (!obj) {
    return 0;
  }
  int off = find_field_off(objectClass(0, obj), "value", -1, 1);
  if (off < 0) {
    return 0;
  }
  GcClass* c = objectClass(0, obj);
  object table = field_table(c);
  unsigned n = array_len(table);
  int code = -1;
  int guard = 0;
  for (GcClass* k = c; k && guard < 64 && code < 0; k = k->super(), ++guard) {
    table = field_table(k);
    n = array_len(table);
    for (unsigned i = 0; i < n; ++i) {
      GcField* f = reinterpret_cast<GcField*>(array_at(table, i));
      if (f && !(f->flags() & ACC_STATIC) && f->offset() == off
          && raw_is(f->name(), "value")) {
        code = f->code();
        break;
      }
    }
  }
  if (code < 0 || code == ObjectField) {
    return 0;
  }
  *ok = 1;
  return static_cast<int64_t>(read_prim_at(obj, code, off));
}

void store_prim_field(object obj, FieldPlan* f, Val v)
{
  if (v.tag == T_NULL) {
    return;
  }
  int64_t bits = static_cast<int64_t>(v.bits);
  if (v.obj && v.tag != T_BOOL && v.tag != T_INT && v.tag != T_LONG && v.tag != T_FLOAT
      && v.tag != T_DOUBLE) {
    int ok = 0;
    bits = unbox_value(v.obj, &ok);
    if (!ok) {
      return;
    }
  }
  unsigned off = static_cast<unsigned>(f->offset);
  switch (f->code) {
  case BooleanField:
    fieldAtOffset<uint8_t>(obj, off) = bits ? 1 : 0;
    break;
  case ByteField:
    fieldAtOffset<int8_t>(obj, off) = static_cast<int8_t>(bits);
    break;
  case CharField:
    fieldAtOffset<uint16_t>(obj, off) = static_cast<uint16_t>(bits);
    break;
  case ShortField:
    fieldAtOffset<int16_t>(obj, off) = static_cast<int16_t>(bits);
    break;
  case IntField:
    fieldAtOffset<int32_t>(obj, off) = static_cast<int32_t>(bits);
    break;
  case LongField:
    if (v.tag == T_INT) {
      bits = static_cast<int32_t>(v.bits);
    }
    fieldAtOffset<int64_t>(obj, off) = bits;
    break;
  case FloatField:
    fieldAtOffset<uint32_t>(obj, off) = static_cast<uint32_t>(bits);
    break;
  case DoubleField:
    fieldAtOffset<uint64_t>(obj, off) = static_cast<uint64_t>(bits);
    break;
  default:
    break;
  }
}

object as_object(Dec* d, Val v)
{
  if (v.obj) {
    return v.obj;
  }
  switch (v.tag) {
  case T_NULL:
    return 0;
  case T_BOOL:
    return box_bool(d, static_cast<uint32_t>(v.bits));
  case T_INT:
    return box_int(d, static_cast<int32_t>(v.bits));
  case T_LONG:
    return box_long(d, v.bits);
  case T_FLOAT:
    return box_float(d, static_cast<uint32_t>(v.bits));
  case T_DOUBLE:
    return box_double(d, v.bits);
  default:
    fail(d->t, 0, "bad state tag");
  }
}

bool slice_eq(Slice s, const char* text, uint32_t n)
{
  if (s.n != n) {
    return false;
  }
  return n == 0 || memcmp(s.p, text, n) == 0;
}

object dentry(Thread* t, Plan* plan)
{
  Dec* d = g_dec;
  if (!d) {
    fail(t, 0, "bad state payload");
  }
  uint32_t count = read_u32(d);
  object obj = d->current;
  for (uint32_t i = 0; i < count; ++i) {
    Slice owner = read_slice(d);
    Slice fname = read_slice(d);
    Slice desc = read_slice(d);
    (void)desc;
    Val value = read_value(d);
    int idx = -1;
    if (i < static_cast<uint32_t>(plan->fieldCount)
        && slice_eq(fname, plan->fields[i].name, plan->fields[i].nameN)
        && slice_eq(owner, plan->fields[i].owner, plan->fields[i].ownerN)) {
      idx = static_cast<int>(i);
    } else {
      for (int f = 0; f < plan->fieldCount; ++f) {
        if (slice_eq(fname, plan->fields[f].name, plan->fields[f].nameN)
            && slice_eq(owner, plan->fields[f].owner, plan->fields[f].ownerN)) {
          idx = f;
          break;
        }
      }
    }
    if (idx < 0) {
      continue;
    }
    FieldPlan* field = &plan->fields[idx];
    if (field->code == ObjectField) {
      object ref = as_object(d, value);
      PROTECT(t, ref);
      setField(t, obj, static_cast<unsigned>(field->offset), ref);
      obj = d->current;
    } else {
      store_prim_field(obj, field, value);
    }
  }
  return d->current;
}

GcString* make_mutf8_string(Dec* d, Slice utf)
{
  if (utf.n > 0x7fffffffu / 3) {
    fail(d->t, 0, "bad state payload");
  }
  unsigned chars = 0;
  uint32_t mlen = 0;
  const uint8_t* cur = utf.p;
  const uint8_t* end = utf.p + utf.n;
  while (cur < end) {
    uint32_t cp;
    if (!next_utf8(cur, end, &cp)) {
      break;
    }
    uint8_t tmp[8];
    unsigned nchars = 0;
    mlen += emit_mutf8(tmp, cp, &nchars);
    chars += nchars;
  }
  (void)chars;
  Thread* t = d->t;
  GcByteArray* bytes = makeByteArray(t, mlen);
  PROTECT(t, bytes);
  uint32_t at = 0;
  cur = utf.p;
  while (cur < end) {
    uint32_t cp;
    if (!next_utf8(cur, end, &cp)) {
      break;
    }
    uint8_t tmp[8];
    unsigned nchars = 0;
    unsigned n = emit_mutf8(tmp, cp, &nchars);
    memcpy(bytes->body().begin() + at, tmp, n);
    at += n;
  }
  GcString* s = makeString(
      t, static_cast<object>(bytes), 0, static_cast<int32_t>(mlen), 0);
  PROTECT(t, s);
  add_id_dec(d, s);
  return s;
}

object read_enum(Dec* d)
{
  Slice typeName = read_slice(d);
  Slice constName = read_slice(d);
  GcClass* c = resolve_dotted(d, typeName, 1);
  if (!c) {
    fail(d->t, 0, "bad state payload");
  }
  PROTECT(d->t, c);
  initClass(d->t, c);
  check_exc(d->t);
  int nameOff = find_field_off(c, "name", ObjectField, 1);
  object table = c->staticTable();
  object fields = field_table(c);
  unsigned n = array_len(fields);
  for (unsigned i = 0; i < n; ++i) {
    GcField* f = reinterpret_cast<GcField*>(array_at(fields, i));
    if (!f || !(f->flags() & ACC_STATIC) || f->code() != ObjectField) {
      continue;
    }
    if (!table) {
      continue;
    }
    object val = fieldAtOffset<object>(table, f->offset());
    if (!val) {
      continue;
    }
    GcClass* oc = objectClass(d->t, val);
    if (oc != c && (!oc || oc->super() != c)) {
      continue;
    }
    if (nameOff < 0) {
      continue;
    }
    object nm = fieldAtOffset<object>(val, static_cast<unsigned>(nameOff));
    if (string_equals_utf8(nm, constName.p, constName.n)) {
      PROTECT(d->t, val);
      add_id_dec(d, val);
      return val;
    }
  }
  fail(d->t, 0, "bad state payload");
}

void store_array_prim(object array, int code, uint32_t index, int64_t bits)
{
  switch (code) {
  case BooleanField:
    fieldAtOffset<uint8_t>(array, ArrayBody + index) = bits ? 1 : 0;
    break;
  case ByteField:
    fieldAtOffset<int8_t>(array, ArrayBody + index) = static_cast<int8_t>(bits);
    break;
  case CharField:
    fieldAtOffset<uint16_t>(array, ArrayBody + index * 2)
        = static_cast<uint16_t>(bits);
    break;
  case ShortField:
    fieldAtOffset<int16_t>(array, ArrayBody + index * 2)
        = static_cast<int16_t>(bits);
    break;
  case IntField:
    fieldAtOffset<int32_t>(array, ArrayBody + index * 4)
        = static_cast<int32_t>(bits);
    break;
  case FloatField:
    fieldAtOffset<uint32_t>(array, ArrayBody + index * 4)
        = static_cast<uint32_t>(bits);
    break;
  case LongField:
    fieldAtOffset<int64_t>(array, ArrayBody + index * 8) = bits;
    break;
  case DoubleField:
    fieldAtOffset<uint64_t>(array, ArrayBody + index * 8)
        = static_cast<uint64_t>(bits);
    break;
  default:
    break;
  }
}

int64_t val_prim_bits(Dec* d, Val v, int code, int* ok)
{
  *ok = 0;
  if (v.tag == T_NULL) {
    fail(d->t, 0, "null primitive");
  }
  if (v.obj && v.tag != T_BOOL && v.tag != T_INT && v.tag != T_LONG && v.tag != T_FLOAT
      && v.tag != T_DOUBLE) {
    return unbox_value(v.obj, ok);
  }
  *ok = 1;
  if (v.tag == T_INT && code == LongField) {
    return static_cast<int32_t>(v.bits);
  }
  return static_cast<int64_t>(v.bits);
}

object read_array(Dec* d)
{
  Slice comp = read_slice(d);
  uint32_t count = read_u32(d);
  if (count > 0x7fffffffu) {
    fail(d->t, 0, "bad state payload");
  }
  Thread* t = d->t;
  int code = -1;
  if (comp.n == 7 && memcmp(comp.p, "boolean", 7) == 0) code = BooleanField;
  else if (comp.n == 4 && memcmp(comp.p, "byte", 4) == 0) code = ByteField;
  else if (comp.n == 4 && memcmp(comp.p, "char", 4) == 0) code = CharField;
  else if (comp.n == 5 && memcmp(comp.p, "short", 5) == 0) code = ShortField;
  else if (comp.n == 3 && memcmp(comp.p, "int", 3) == 0) code = IntField;
  else if (comp.n == 4 && memcmp(comp.p, "long", 4) == 0) code = LongField;
  else if (comp.n == 5 && memcmp(comp.p, "float", 5) == 0) code = FloatField;
  else if (comp.n == 6 && memcmp(comp.p, "double", 6) == 0) code = DoubleField;
  object array = 0;
  if (code >= 0) {
    if (code == BooleanField) array = makeBooleanArray(t, count);
    else if (code == ByteField) array = makeByteArray(t, count);
    else if (code == CharField) array = makeCharArray(t, count);
    else if (code == ShortField) array = makeShortArray(t, count);
    else if (code == IntField) array = makeIntArray(t, count);
    else if (code == LongField) array = makeLongArray(t, count);
    else if (code == FloatField) array = makeFloatArray(t, count);
    else array = makeDoubleArray(t, count);
    PROTECT(t, array);
    add_id_dec(d, array);
    for (uint32_t i = 0; i < count; ++i) {
      Val v = read_value(d);
      int ok = 0;
      int64_t bits = val_prim_bits(d, v, code, &ok);
      if (!ok) {
        fail(d->t, 0, "null primitive");
      }
      store_array_prim(array, code, i, bits);
      array = id_at(d, d->idCount - 1);
    }
    return id_at(d, d->idCount - 1);
  }
  GcClass* compC = resolve_dotted(d, comp, 1);
  if (!compC) {
    fail(d->t, 0, "bad state payload");
  }
  PROTECT(t, compC);
  array = makeObjectArray(t, compC, count);
  PROTECT(t, array);
  add_id_dec(d, array);
  uint32_t id = d->idCount - 1;
  for (uint32_t i = 0; i < count; ++i) {
    Val v = read_value(d);
    object elem = as_object(d, v);
    PROTECT(t, elem);
    array = id_at(d, id);
    reinterpret_cast<GcArray*>(array)->setBodyElement(t, i, elem);
  }
  return id_at(d, id);
}

GcMethod* declared_init(GcClass* c)
{
  object table = c ? c->methodTable() : 0;
  unsigned n = array_len(table);
  for (unsigned i = 0; i < n; ++i) {
    GcMethod* m = reinterpret_cast<GcMethod*>(array_at(table, i));
    if (!m || !m->name() || !m->spec()) {
      continue;
    }
    if (raw_is(m->name(), "<init>") && raw_is(m->spec(), "()V")) {
      return m;
    }
  }
  return 0;
}

class CatchPoint : public Thread::Checkpoint {
 public:
  explicit CatchPoint(Thread* thread) : Checkpoint(thread) {}
  jmp_buf buf;
  virtual void NO_RETURN unwind() { longjmp(buf, 1); }
};

bool invoke_ok(Thread* t, GcMethod* method, object self, object a, object b, int argc)
{
  volatile uintptr_t threadBits = reinterpret_cast<uintptr_t>(t);
  volatile uintptr_t methodBits = reinterpret_cast<uintptr_t>(method);
  volatile uintptr_t selfBits = reinterpret_cast<uintptr_t>(self);
  volatile uintptr_t aBits = reinterpret_cast<uintptr_t>(a);
  volatile uintptr_t bBits = reinterpret_cast<uintptr_t>(b);
  volatile int argcBits = argc;
  CatchPoint point(t);
  if (setjmp(point.buf)) {
    reinterpret_cast<Thread*>(static_cast<uintptr_t>(threadBits))->exception = 0;
    return false;
  }
  Thread* thread = reinterpret_cast<Thread*>(static_cast<uintptr_t>(threadBits));
  GcMethod* m = reinterpret_cast<GcMethod*>(static_cast<uintptr_t>(methodBits));
  object receiver = reinterpret_cast<object>(static_cast<uintptr_t>(selfBits));
  object arg0 = reinterpret_cast<object>(static_cast<uintptr_t>(aBits));
  object arg1 = reinterpret_cast<object>(static_cast<uintptr_t>(bBits));
  if (argcBits <= 0) {
    thread->m->processor->invoke(thread, m, receiver);
  } else if (argcBits == 1) {
    thread->m->processor->invoke(thread, m, receiver, arg0);
  } else {
    thread->m->processor->invoke(thread, m, receiver, arg0, arg1);
  }
  if (thread->exception) {
    thread->exception = 0;
    return false;
  }
  return true;
}

object call_empty(Dec* d, int slot, const char* name, const char* spec)
{
  GcMethod* m = cache_method(d, slot, "java/util/Collections", name, spec);
  if (d->t->exception) {
    d->t->exception = 0;
  }
  object r = d->t->m->processor->invoke(d->t, m, 0);
  check_exc(d->t);
  return r;
}

object fallback_coll(Dec* d, int as_set, int as_map)
{
  const char* cls = as_map ? "java/util/HashMap"
                            : as_set ? "java/util/HashSet" : "java/util/ArrayList";
  GcClass* c = resolve_internal(d->t, cls, 1);
  PROTECT(d->t, c);
  initClass(d->t, c);
  GcMethod* init = declared_init(c);
  object obj = make(d->t, c);
  PROTECT(d->t, obj);
  if (init) {
    if (d->t->exception) d->t->exception = 0;
    d->t->m->processor->invoke(d->t, init, obj);
    if (d->t->exception) {
      d->t->exception = 0;
    }
  }
  return obj;
}

object allocate_coll(Dec* d, Slice name, int as_set, int as_map, int* emptySpecial)
{
  *emptySpecial = 0;
  if (slice_has(name, "EmptyList")) {
    *emptySpecial = 1;
    return call_empty(d, K_EMPTY_LIST, "emptyList", "()Ljava/util/List;");
  }
  if (slice_has(name, "EmptySet")) {
    *emptySpecial = 2;
    return call_empty(d, K_EMPTY_SET, "emptySet", "()Ljava/util/Set;");
  }
  if (slice_has(name, "EmptyMap")) {
    *emptySpecial = 3;
    return call_empty(d, K_EMPTY_MAP, "emptyMap", "()Ljava/util/Map;");
  }
  GcClass* c = resolve_dotted(d, name, 0);
  if (d->t->exception) {
    d->t->exception = 0;
    c = 0;
  }
  if (!c) {
    return fallback_coll(d, as_set, as_map);
  }
  PROTECT(d->t, c);
  initClass(d->t, c);
  check_exc(d->t);
  GcMethod* init = declared_init(c);
  object obj = make(d->t, c);
  PROTECT(d->t, obj);
  if (init) {
    if (!invoke_ok(d->t, init, obj, 0, 0, 0)) {
      obj = make(d->t, c);
    }
  }
  if (!obj) {
    return fallback_coll(d, as_set, as_map);
  }
  return obj;
}

void fill_fields(Dec* d, object obj, object elems, uint32_t count)
{
  PROTECT(d->t, obj);
  GcClass* c = objectClass(d->t, obj);
  object table = field_table(c);
  unsigned n = array_len(table);
  uint32_t used = 0;
  for (unsigned i = 0; i < n && used < count; ++i) {
    GcField* f = reinterpret_cast<GcField*>(array_at(table, i));
    if (!f || (f->flags() & ACC_STATIC)) {
      continue;
    }
    object elem = reinterpret_cast<GcArray*>(elems)->body()[used++];
    PROTECT(d->t, elem);
    obj = d->current ? d->current : obj;
    if (f->code() == ObjectField) {
      setField(d->t, obj, f->offset(), elem);
    } else {
      int ok = 0;
      int64_t bits = unbox_value(elem, &ok);
      if (ok) {
        FieldPlan tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.offset = f->offset();
        tmp.code = f->code();
        Val v;
        memset(&v, 0, sizeof v);
        v.tag = T_LONG;
        v.bits = static_cast<uint64_t>(bits);
        if (f->code() == FloatField || f->code() == IntField || f->code() == BooleanField
            || f->code() == ByteField || f->code() == CharField || f->code() == ShortField) {
          v.tag = T_INT;
        }
        if (f->code() == DoubleField) {
          v.tag = T_DOUBLE;
        }
        if (f->code() == FloatField) {
          v.tag = T_FLOAT;
        }
        store_prim_field(obj, &tmp, v);
      }
    }
  }
}

object read_collection(Dec* d, int as_set)
{
  Slice name = read_slice(d);
  uint32_t count = read_u32(d);
  if (count > 0x7fffffffu) {
    fail(d->t, 0, "bad state payload");
  }
  int emptySpecial = 0;
  object obj = allocate_coll(d, name, as_set, 0, &emptySpecial);
  if (!obj) {
    fail(d->t, 0, "bad state payload");
  }
  PROTECT(d->t, obj);
  add_id_dec(d, obj);
  uint32_t id = d->idCount - 1;
  object elems = makeObjectArray(d->t, count);
  PROTECT(d->t, elems);
  for (uint32_t i = 0; i < count; ++i) {
    Val v = read_value(d);
    object elem = as_object(d, v);
    PROTECT(d->t, elem);
    elems = reinterpret_cast<GcArray*>(d->cache) ? elems : elems;
    reinterpret_cast<GcArray*>(elems)->setBodyElement(d->t, i, elem);
  }
  obj = id_at(d, id);
  if (emptySpecial) {
    return obj;
  }
  GcClass* oc = objectClass(d->t, obj);
  GcMethod* add = findMethodOrNull(d->t, oc, "add", "(Ljava/lang/Object;)Z");
  PROTECT(d->t, add);
  int failed = 0;
  if (add) {
    for (uint32_t i = 0; i < count; ++i) {
      object elem = reinterpret_cast<GcArray*>(elems)->body()[i];
      PROTECT(d->t, elem);
      obj = id_at(d, id);
      if (!invoke_ok(d->t, add, obj, elem, 0, 1)) {
        failed = 1;
        break;
      }
    }
  } else {
    failed = 1;
  }
  if (failed) {
    obj = id_at(d, id);
    d->current = obj;
    fill_fields(d, obj, elems, count);
  }
  return id_at(d, id);
}

object read_map(Dec* d)
{
  Slice name = read_slice(d);
  uint32_t count = read_u32(d);
  if (count > 0x7fffffffu) {
    fail(d->t, 0, "bad state payload");
  }
  int emptySpecial = 0;
  object obj = allocate_coll(d, name, 0, 1, &emptySpecial);
  if (!obj) {
    fail(d->t, 0, "bad state payload");
  }
  PROTECT(d->t, obj);
  add_id_dec(d, obj);
  uint32_t id = d->idCount - 1;
  object flat = makeObjectArray(d->t, count ? count * 2 : 0);
  PROTECT(d->t, flat);
  uint32_t flatN = 0;
  int putOk = emptySpecial != 3;
  GcMethod* put = 0;
  if (putOk) {
    put = findMethodOrNull(d->t,
                           objectClass(d->t, obj),
                           "put",
                           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    if (!put) {
      putOk = 0;
    }
  }
  PROTECT(d->t, put);
  for (uint32_t i = 0; i < count; ++i) {
    Val vk = read_value(d);
    object key = as_object(d, vk);
    PROTECT(d->t, key);
    Val vv = read_value(d);
    object val = as_object(d, vv);
    PROTECT(d->t, val);
    obj = id_at(d, id);
    if (putOk) {
      if (!invoke_ok(d->t, put, obj, key, val, 2)) {
        putOk = 0;
        reinterpret_cast<GcArray*>(flat)->setBodyElement(d->t, flatN++, key);
        reinterpret_cast<GcArray*>(flat)->setBodyElement(d->t, flatN++, val);
      }
    } else {
      reinterpret_cast<GcArray*>(flat)->setBodyElement(d->t, flatN++, key);
      reinterpret_cast<GcArray*>(flat)->setBodyElement(d->t, flatN++, val);
    }
  }
  if (flatN) {
    obj = id_at(d, id);
    d->current = obj;
    fill_fields(d, obj, flat, flatN);
  }
  return id_at(d, id);
}

object read_object(Dec* d)
{
  Slice typeName = read_slice(d);
  GcClass* c = resolve_dotted(d, typeName, 1);
  if (!c) {
    check_exc(d->t);
    fail(d->t, 0, "bad state payload");
  }
  PROTECT(d->t, c);
  initClass(d->t, c);
  check_exc(d->t);
  object obj = make(d->t, c);
  PROTECT(d->t, obj);
  object saved = d->current;
  PROTECT(d->t, saved);
  d->current = obj;
  add_id_dec(d, obj);
  ensure_fn(d->t, c);
  typedef object (*Fn)(Thread*);
  Fn fn = reinterpret_cast<Fn>(static_cast<uintptr_t>(
      __atomic_load_n(&c->deserializeThunk(), __ATOMIC_ACQUIRE)));
  if (!fn) {
    fail(d->t, 0, "bad state payload");
  }
  fn(d->t);
  object result = d->current;
  d->current = saved;
  return result;
}

Val read_body(Dec* d)
{
  uint8_t tag = read_u8(d);
  Val v;
  memset(&v, 0, sizeof v);
  v.tag = tag;
  switch (tag) {
  case T_NULL:
    return v;
  case T_BOOL:
    v.bits = read_u8(d) ? 1 : 0;
    return v;
  case T_INT:
    v.bits = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(read_u32(d))));
    return v;
  case T_LONG:
    v.bits = read_u64(d);
    return v;
  case T_FLOAT:
    v.bits = read_u32(d);
    return v;
  case T_DOUBLE:
    v.bits = read_u64(d);
    return v;
  case T_BACKREF:
    v.obj = id_at(d, read_u32(d));
    return v;
  case T_STRING:
    v.obj = make_mutf8_string(d, read_slice(d));
    return v;
  case T_CLASS: {
    Slice name = read_slice(d);
    GcClass* c = resolve_dotted(d, name, 1);
    if (!c) {
      fail(d->t, 0, "bad state payload");
    }
    PROTECT(d->t, c);
    GcJclass* j = getJClass(d->t, c);
    PROTECT(d->t, j);
    add_id_dec(d, j);
    v.obj = j;
    return v;
  }
  case T_ENUM:
    v.obj = read_enum(d);
    return v;
  case T_ARRAY:
    v.obj = read_array(d);
    return v;
  case T_LIST:
    v.obj = read_collection(d, 0);
    return v;
  case T_SET:
    v.obj = read_collection(d, 1);
    return v;
  case T_MAP:
    v.obj = read_map(d);
    return v;
  case T_OBJECT:
    v.obj = read_object(d);
    return v;
  default:
    fail(d->t, 0, "bad state tag");
  }
}

Val read_value(Dec* d)
{
  ++d->depth;
  if (d->depth > static_cast<int>(kMaxDepth)) {
    fail(d->t, 0, "state graph is too deep");
  }
  Val v = read_body(d);
  --d->depth;
  return v;
}

uint64_t serialize_run(Thread* t, uintptr_t* arguments)
{
  poison();
  g_walk = 0;
  g_dec = 0;
  jobject ref = reinterpret_cast<jobject>(arguments[0]);
  object obj = (ref == 0 || *ref == 0) ? 0 : *ref;
  if (!obj) {
    GcByteArray* a = makeByteArray(t, 2);
    a->body()[0] = 1;
    a->body()[1] = T_NULL;
    PROTECT(t, a);
    return reinterpret_cast<uint64_t>(makeLocalReference(t, a));
  }
  PROTECT(t, obj);
  GcClass* mapC = boot_named(t, "java/util/Map", &g_slot_map);
  PROTECT(t, mapC);
  GcClass* colC = boot_named(t, "java/util/Collection", &g_slot_col);
  PROTECT(t, colC);
  GcClass* setC = boot_named(t, "java/util/Set", &g_slot_set);
  PROTECT(t, setC);
  g_mapC = mapC;
  g_colC = colC;
  g_setC = setC;
  g_hot = 1;
  GcClass* c = objectClass(t, obj);
  ensure_fn(t, c);
  SerFn fn = reinterpret_cast<SerFn>(static_cast<uintptr_t>(
      __atomic_load_n(&c->serializeThunk(), __ATOMIC_ACQUIRE)));
  GcByteArray* bytes = fn(t, obj);
  PROTECT(t, bytes);
  poison();
  return reinterpret_cast<uint64_t>(makeLocalReference(t, bytes));
}

object ref_obj(jobject ref)
{
  return (ref == 0 || *ref == 0) ? 0 : *ref;
}

void copy_plain(Thread* t, const uint8_t* p, unsigned n, int slash, uint8_t** out, uint32_t* outN)
{
  uint8_t* dst = static_cast<uint8_t*>(malloc(static_cast<size_t>(n) + 1));
  if (!dst) {
    fail(t, 1, "OutOfMemoryError");
  }
  if (n != 0) {
    memcpy(dst, p, n);
    if (slash) {
      for (unsigned i = 0; i < n; ++i) {
        if (dst[i] == '/') dst[i] = '.';
      }
    }
  }
  dst[n] = 0;
  *out = dst;
  *outN = n;
}

void copy_text(Thread* t, const uint8_t* p, unsigned n, int slash, uint8_t** out, uint32_t* outN)
{
  if (plain_ascii(p, n)) {
    copy_plain(t, p, n, slash, out, outN);
    return;
  }
  uint32_t len = 0;
  char* buf = utf8_from_mutf8(t, p, n, &len, slash);
  *out = reinterpret_cast<uint8_t*>(buf);
  *outN = len;
}

void copy_string_obj(Thread* t, object str, uint8_t** out, uint32_t* outN)
{
  *out = 0;
  *outN = 0;
  if (!str) {
    return;
  }
  const uint8_t* p;
  unsigned n = 0;
  string_bytes(str, p, n);
  copy_text(t, p, n, 0, out, outN);
}

void copy_class_name(Thread* t, object cls, uint8_t** out, uint32_t* outN)
{
  *out = 0;
  *outN = 0;
  if (!cls) {
    fail(t, 0, "state type is null");
  }
  GcClass* vm = cast<GcJclass>(t, cls)->vmClass();
  const char* prim = primitive_name(t, vm);
  if (prim) {
    copy_plain(t, reinterpret_cast<const uint8_t*>(prim), static_cast<unsigned>(strlen(prim)), 0, out, outN);
    return;
  }
  object bin = vm ? reinterpret_cast<object>(vm->binaryName()) : 0;
  if (bin) {
    copy_string_obj(t, bin, out, outN);
    return;
  }
  copy_text(t, bytes_p(vm ? vm->name() : 0), bytes_n(vm ? vm->name() : 0), 1, out, outN);
}

uint64_t capture_run(Thread* t, uintptr_t* arguments)
{
  poison();
  g_walk = 0;
  g_dec = 0;
  g_kept = 0;
  g_kept_n = 0;
  StateCapture* out = reinterpret_cast<StateCapture*>(arguments[4]);
  memset(out, 0, sizeof *out);
  object plugin = ref_obj(reinterpret_cast<jobject>(arguments[0]));
  object type = ref_obj(reinterpret_cast<jobject>(arguments[1]));
  object name = ref_obj(reinterpret_cast<jobject>(arguments[2]));
  object state = ref_obj(reinterpret_cast<jobject>(arguments[3]));
  copy_string_obj(t, plugin, &out->plugin, &out->plugin_n);
  copy_class_name(t, type, &out->type, &out->type_n);
  if (name) {
    out->has_name = 1;
    copy_string_obj(t, name, &out->name, &out->name_n);
  }
  if (!state) {
    uint8_t* body = static_cast<uint8_t*>(malloc(2));
    if (!body) {
      fail(t, 1, "OutOfMemoryError");
    }
    body[0] = 1;
    body[1] = T_NULL;
    out->body = body;
    out->body_n = 2;
    return 1;
  }
  PROTECT(t, state);
  GcClass* mapC = boot_named(t, "java/util/Map", &g_slot_map);
  PROTECT(t, mapC);
  GcClass* colC = boot_named(t, "java/util/Collection", &g_slot_col);
  PROTECT(t, colC);
  GcClass* setC = boot_named(t, "java/util/Set", &g_slot_set);
  PROTECT(t, setC);
  g_mapC = mapC;
  g_colC = colC;
  g_setC = setC;
  g_hot = 1;
  g_keep = 1;
  GcClass* c = objectClass(t, state);
  ensure_fn(t, c);
  SerFn fn = reinterpret_cast<SerFn>(static_cast<uintptr_t>(
      __atomic_load_n(&c->serializeThunk(), __ATOMIC_ACQUIRE)));
  fn(t, state);
  out->body = g_kept;
  out->body_n = g_kept_n;
  g_kept = 0;
  g_kept_n = 0;
  poison();
  return 1;
}

uint64_t deserialize_run(Thread* t, uintptr_t* arguments)
{
  poison();
  g_walk = 0;
  g_dec = 0;
  const uint8_t* data = reinterpret_cast<const uint8_t*>(arguments[0]);
  jint length = static_cast<jint>(arguments[1]);
  if (!data || length < 2 || data[0] != 1) {
    fail(t, 0, "bad state payload");
  }
  Dec dec;
  memset(&dec, 0, sizeof dec);
  dec.t = t;
  dec.p = data + 1;
  dec.end = data + length;
  dec.ids = makeObjectArray(t, 16);
  dec.idCap = 16;
  dec.cache = makeObjectArray(t, K_CACHE_N);
  PROTECT(t, dec.ids);
  PROTECT(t, dec.current);
  PROTECT(t, dec.cache);
  g_dec = &dec;
  Val v = read_value(&dec);
  object obj = as_object(&dec, v);
  PROTECT(t, obj);
  g_dec = 0;
  poison();
  return reinterpret_cast<uint64_t>(makeLocalReference(t, obj));
}

}  // namespace

jbyteArray JNICALL SerializeGraph(Thread* t, jobject object)
{
  uintptr_t arguments[] = {reinterpret_cast<uintptr_t>(object)};
  return reinterpret_cast<jbyteArray>(run(t, serialize_run, arguments));
}

jint JNICALL CaptureState(Thread* t,
                          jstring plugin,
                          jobject type,
                          jstring name,
                          jobject state,
                          StateCapture* out)
{
  if (!out) {
    return -1;
  }
  uintptr_t arguments[] = {reinterpret_cast<uintptr_t>(plugin),
                           reinterpret_cast<uintptr_t>(type),
                           reinterpret_cast<uintptr_t>(name),
                           reinterpret_cast<uintptr_t>(state),
                           reinterpret_cast<uintptr_t>(out)};
  run(t, capture_run, arguments);
  if (t->exception) {
    return -1;
  }
  return 0;
}

jobject JNICALL DeserializeGraph(Thread* t, const uint8_t* data, jint length)
{
  uintptr_t arguments[] = {reinterpret_cast<uintptr_t>(data),
                           static_cast<uintptr_t>(length)};
  return reinterpret_cast<jobject>(run(t, deserialize_run, arguments));
}

}  // namespace vm
