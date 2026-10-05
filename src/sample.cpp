/* CPU samples of JIT and boot code, off unless AVIAN_SAMPLE=1.
   The handler only stores instruction pointers. Names are copied at
   compile time into malloc memory, so the printer never touches the heap. */

#define _GNU_SOURCE
#include <atomic>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

namespace {

const unsigned kSampleCap = 1 << 20;
const unsigned kTop = 40;

struct Range {
  uintptr_t start;
  uintptr_t end;
  const char* name;
};

bool enabled = false;
pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
Range* ranges = 0;
uint32_t rangeCount = 0;
uint32_t rangeCap = 0;

uintptr_t samples[kSampleCap];
std::atomic<uint32_t> sampleCount(0);
std::atomic<uint32_t> armedThreads(0);

// Return addresses above a libc wait, walked from the frame pointer.
const unsigned kStackCap = 1 << 16;
const unsigned kFrames = 8;
uintptr_t stacks[kStackCap][kFrames];
uintptr_t libcLo[8];
uintptr_t libcHi[8];
unsigned libcMaps = 0;

void noteLibcMaps()
{
  FILE* maps = fopen("/proc/self/maps", "r");
  if (maps == 0) {
    return;
  }
  char line[512];
  while (fgets(line, sizeof line, maps) != 0 and libcMaps < 8) {
    if (strstr(line, "libc.so") == 0 or strstr(line, "r-xp") == 0) {
      continue;
    }
    unsigned long lo = 0;
    unsigned long hi = 0;
    if (sscanf(line, "%lx-%lx", &lo, &hi) == 2) {
      libcLo[libcMaps] = lo;
      libcHi[libcMaps] = hi;
      ++libcMaps;
    }
  }
  fclose(maps);
}

bool ipInLibc(uintptr_t ip)
{
  for (unsigned i = 0; i < libcMaps; ++i) {
    if (ip >= libcLo[i] and ip < libcHi[i]) {
      return true;
    }
  }
  return false;
}

int rangeCmp(const void* a, const void* b)
{
  const Range* ra = static_cast<const Range*>(a);
  const Range* rb = static_cast<const Range*>(b);
  if (ra->start < rb->start) {
    return -1;
  }
  if (ra->start > rb->start) {
    return 1;
  }
  return 0;
}

const Range* findRange(uintptr_t ip)
{
  uint32_t lo = 0;
  uint32_t hi = rangeCount;
  while (lo < hi) {
    uint32_t mid = lo + ((hi - lo) / 2);
    if (ranges[mid].start <= ip) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) {
    return 0;
  }
  const Range* r = ranges + (lo - 1);
  if (ip < r->end) {
    return r;
  }
  return 0;
}

struct Hit {
  const char* name;
  uint32_t count;
};

struct IpHit {
  uintptr_t ip;
  uint32_t count;
};

const unsigned kSlots = 8192;
Hit hits[kSlots];

uint32_t hashName(const char* s)
{
  uint32_t h = 2166136261u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p;
       ++p) {
    h ^= *p;
    h *= 16777619u;
  }
  return h;
}

void addHit(const char* name)
{
  uint32_t i = hashName(name) & (kSlots - 1);
  for (unsigned n = 0; n < kSlots; ++n) {
    Hit* h = hits + i;
    if (h->name == 0) {
      h->name = name;
      h->count = 1;
      return;
    }
    if (strcmp(h->name, name) == 0) {
      ++h->count;
      return;
    }
    i = (i + 1) & (kSlots - 1);
  }
}

void onSample(int, siginfo_t*, void* context)
{
  ucontext_t* uc = static_cast<ucontext_t*>(context);
  uintptr_t ip = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
  uint32_t i = sampleCount.fetch_add(1, std::memory_order_relaxed);
  if (i < kSampleCap) {
    samples[i] = ip;
  }
  // backtrace() from this handler aborts when it reenters libgcc.
  (void)i;
  (void)ip;
}

const unsigned kIpSlots = 1 << 15;
IpHit* ipHits = 0;

void addIp(uintptr_t ip)
{
  if (ipHits == 0) {
    return;
  }
  uint32_t x = static_cast<uint32_t>(ip >> 2);
  x ^= x >> 16;
  x *= 0x7feb352du;
  uint32_t slot = x & (kIpSlots - 1);
  for (unsigned probe = 0; probe < 128; ++probe) {
    IpHit* h = ipHits + slot;
    if (h->ip == 0) {
      h->ip = ip;
      h->count = 1;
      return;
    }
    if (h->ip == ip) {
      ++h->count;
      return;
    }
    slot = (slot + 1) & (kIpSlots - 1);
  }
}

void dumpSamples()
{
  uint32_t n = sampleCount.load(std::memory_order_relaxed);
  if (n > kSampleCap) {
    n = kSampleCap;
  }
  if (ipHits == 0) {
    ipHits = static_cast<IpHit*>(calloc(kIpSlots, sizeof(IpHit)));
  }
  pthread_mutex_lock(&mu);
  qsort(ranges, rangeCount, sizeof(Range), rangeCmp);
  memset(hits, 0, sizeof hits);
  if (ipHits != 0) {
    memset(ipHits, 0, kIpSlots * sizeof(IpHit));
  }
  uint32_t unknown = 0;
  uint32_t seen = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uintptr_t ip = samples[i];
    if (ip == 0) {
      continue;
    }
    ++seen;
    const Range* r = findRange(ip);
    if (r == 0) {
      ++unknown;
      addIp(ip);
    } else {
      addHit(r->name);
    }
  }
  Hit top[kTop];
  memset(top, 0, sizeof top);
  for (unsigned s = 0; s < kSlots; ++s) {
    if (hits[s].count == 0) {
      continue;
    }
    for (unsigned t = 0; t < kTop; ++t) {
      if (hits[s].count > top[t].count) {
        for (unsigned u = kTop - 1; u > t; --u) {
          top[u] = top[u - 1];
        }
        top[t] = hits[s];
        break;
      }
    }
  }
  unsigned methods = rangeCount;
  pthread_mutex_unlock(&mu);

  // Unknown instruction pointers are native code or unpublished JIT.
  // Group by loaded object here; a side file keeps offsets for addr2line.
  struct Lib {
    const char* name;
    uint32_t count;
  };
  Lib libs[12];
  memset(libs, 0, sizeof libs);
  const char* ipPath = getenv("AVIAN_SAMPLE_IPS");
  FILE* ipFile = 0;
  if (ipPath != 0 && ipPath[0] != 0) {
    ipFile = fopen(ipPath, "w");
  }
  if (ipHits != 0) {
    for (unsigned s = 0; s < kIpSlots; ++s) {
      if (ipHits[s].count == 0) {
        continue;
      }
      uintptr_t ip = ipHits[s].ip;
      Dl_info info;
      const char* base = "anon";
      const char* full = "anon";
      uintptr_t off = ip;
      if (dladdr(reinterpret_cast<void*>(ip), &info) != 0 && info.dli_fname != 0) {
        full = info.dli_fname;
        const char* slash = strrchr(info.dli_fname, '/');
        base = slash != 0 ? slash + 1 : info.dli_fname;
        off = ip - reinterpret_cast<uintptr_t>(info.dli_fbase);
      }
      unsigned li;
      for (li = 0; li < 12; ++li) {
        if (libs[li].name == 0) {
          libs[li].name = base;
          libs[li].count = ipHits[s].count;
          break;
        }
        if (strcmp(libs[li].name, base) == 0) {
          libs[li].count += ipHits[s].count;
          break;
        }
      }
      if (ipFile != 0) {
        fprintf(ipFile, "%u %s %lx\n", ipHits[s].count, full, static_cast<unsigned long>(off));
      }
    }
  }
  if (ipFile != 0) {
    fclose(ipFile);
  }
  for (unsigned a = 0; a < 12; ++a) {
    for (unsigned b = a + 1; b < 12; ++b) {
      if (libs[b].count > libs[a].count) {
        Lib tmp = libs[a];
        libs[a] = libs[b];
        libs[b] = tmp;
      }
    }
  }

  fprintf(stderr,
          "[avian] sample seen=%u unknown=%u methods=%u armed=%u\n",
          seen,
          unknown,
          methods,
          armedThreads.load(std::memory_order_relaxed));
  for (unsigned li = 0; li < 12; ++li) {
    if (libs[li].count == 0) {
      break;
    }
    unsigned pct = seen ? (libs[li].count * 100u) / seen : 0;
    fprintf(stderr,
            "[avian] sample lib %u %u%% %s\n",
            libs[li].count,
            pct,
            libs[li].name);
  }
  for (unsigned t = 0; t < kTop; ++t) {
    if (top[t].count == 0) {
      break;
    }
    unsigned pct = seen ? (top[t].count * 100u) / seen : 0;
    fprintf(stderr,
            "[avian] sample %u %u%% %s\n",
            top[t].count,
            pct,
            top[t].name);
  }
  const char* hitPath = getenv("AVIAN_SAMPLE_HITS");
  if (hitPath != 0 && hitPath[0] != 0) {
    FILE* hitFile = fopen(hitPath, "w");
    if (hitFile != 0) {
      for (unsigned s = 0; s < kSlots; ++s) {
        if (hits[s].count == 0) {
          continue;
        }
        fprintf(hitFile, "%u %s\n", hits[s].count, hits[s].name);
      }
      fclose(hitFile);
    }
  }

  // Who blocks in libc. The hottest non-libc return address is the waiter.
  struct Wait {
    uintptr_t ip;
    uintptr_t next;
    uintptr_t deep;
    uint32_t count;
  };
  const unsigned kWaitSlots = 4096;
  static Wait* waits = 0;
  if (waits == 0) {
    waits = static_cast<Wait*>(calloc(kWaitSlots, sizeof(Wait)));
  }
  if (waits != 0) {
    memset(waits, 0, kWaitSlots * sizeof(Wait));
    uint32_t limit = n;
    if (limit > kStackCap) {
      limit = kStackCap;
    }
    for (uint32_t i = 0; i < limit; ++i) {
      uintptr_t ip = samples[i];
      if (ip == 0 or stacks[i][0] == 0) {
        continue;
      }
      Dl_info info;
      if (dladdr(reinterpret_cast<void*>(ip), &info) == 0 or info.dli_fname == 0) {
        continue;
      }
      const char* slash = strrchr(info.dli_fname, '/');
      const char* base = slash != 0 ? slash + 1 : info.dli_fname;
      if (strcmp(base, "libc.so.6") != 0) {
        continue;
      }
      uintptr_t found[3];
      unsigned nfound = 0;
      bool sawLibc = false;
      for (unsigned f = 0; f < kFrames and nfound < 3; ++f) {
        uintptr_t ret = stacks[i][f];
        if (ret == 0) {
          break;
        }
        Dl_info frame;
        if (dladdr(reinterpret_cast<void*>(ret), &frame) == 0 or frame.dli_fname == 0) {
          continue;
        }
        const char* fs = strrchr(frame.dli_fname, '/');
        const char* fb = fs != 0 ? fs + 1 : frame.dli_fname;
        if (strcmp(fb, "libc.so.6") == 0) {
          sawLibc = true;
          continue;
        }
        if (not sawLibc or strcmp(fb, "linux-vdso.so.1") == 0) {
          continue;
        }
        found[nfound++] = ret;
      }
      if (nfound == 0) {
        continue;
      }
      uintptr_t key = nfound > 1 ? found[1] : found[0];
      uint32_t slot = static_cast<uint32_t>(key >> 2) & (kWaitSlots - 1);
      for (unsigned probe = 0; probe < 32; ++probe) {
        if (waits[slot].ip == 0) {
          waits[slot].ip = found[0];
          waits[slot].next = nfound > 1 ? found[1] : 0;
          waits[slot].deep = nfound > 2 ? found[2] : 0;
          waits[slot].count = 1;
          break;
        }
        if (waits[slot].next == (nfound > 1 ? found[1] : 0)
            and waits[slot].ip == found[0]) {
          ++waits[slot].count;
          break;
        }
        slot = (slot + 1) & (kWaitSlots - 1);
      }
    }
    Wait topw[12];
    memset(topw, 0, sizeof topw);
    for (unsigned s = 0; s < kWaitSlots; ++s) {
      if (waits[s].count == 0) {
        continue;
      }
      for (unsigned t = 0; t < 12; ++t) {
        if (waits[s].count > topw[t].count) {
          for (unsigned u = 11; u > t; --u) {
            topw[u] = topw[u - 1];
          }
          topw[t] = waits[s];
          break;
        }
      }
    }
    for (unsigned t = 0; t < 12; ++t) {
      if (topw[t].count == 0) {
        break;
      }
      auto emit = [](uintptr_t ip) {
        if (ip == 0) {
          fprintf(stderr, " -");
          return;
        }
        Dl_info info;
        const char* full = "anon";
        uintptr_t off = ip;
        if (dladdr(reinterpret_cast<void*>(ip), &info) != 0 && info.dli_fname != 0) {
          full = info.dli_fname;
          off = ip - reinterpret_cast<uintptr_t>(info.dli_fbase);
        }
        const char* slash = strrchr(full, '/');
        const char* base = slash != 0 ? slash + 1 : full;
        fprintf(stderr, " %s+0x%lx", base, static_cast<unsigned long>(off));
      };
      fprintf(stderr, "[avian] sample wait %u", topw[t].count);
      emit(topw[t].ip);
      emit(topw[t].next);
      emit(topw[t].deep);
      fprintf(stderr, "\n");
    }
  }
  fflush(stderr);
}

void* printer(void*)
{
  for (;;) {
    sleep(1);
    dumpSamples();
  }
  return 0;
}

struct Enable {
  Enable()
  {
    const char* e = getenv("AVIAN_SAMPLE");
    enabled = e != 0 and e[0] != 0 and e[0] != '0';
    // Install before any thread can arm a timer. The default action of
    // SIGPROF is to kill the process.
    if (enabled) {
      noteLibcMaps();
      struct sigaction sa;
      memset(&sa, 0, sizeof sa);
      sa.sa_sigaction = onSample;
      sa.sa_flags = SA_SIGINFO | SA_RESTART;
      sigemptyset(&sa.sa_mask);
      sigaction(SIGPROF, &sa, 0);
    }
  }
} enable;

}  // namespace

void avianSampleNote(const void* code, unsigned size, const char* label);

void avianSampleNoteParts(const void* code,
                          unsigned size,
                          const char* class_,
                          const char* name,
                          const char* spec)
{
  if (not enabled or code == 0 or size == 0) {
    return;
  }
  char label[512];
  snprintf(label,
           sizeof label,
           "%s.%s%s",
           class_ ? class_ : "",
           name ? name : "",
           spec ? spec : "");
  avianSampleNote(code, size, label);
}

void avianSampleNote(const void* code, unsigned size, const char* label)
{
  if (not enabled or code == 0 or size == 0 or label == 0) {
    return;
  }
  const char* copy = strdup(label);
  if (copy == 0) {
    return;
  }
  pthread_mutex_lock(&mu);
  if (rangeCount == rangeCap) {
    uint32_t cap = rangeCap ? rangeCap * 2 : 1024;
    Range* grown = static_cast<Range*>(realloc(ranges, cap * sizeof(Range)));
    if (grown == 0) {
      pthread_mutex_unlock(&mu);
      return;
    }
    ranges = grown;
    rangeCap = cap;
  }
  ranges[rangeCount].start = reinterpret_cast<uintptr_t>(code);
  ranges[rangeCount].end = reinterpret_cast<uintptr_t>(code) + size;
  ranges[rangeCount].name = copy;
  ++rangeCount;
  pthread_mutex_unlock(&mu);
}

void avianSampleArmThread()
{
  if (not enabled) {
    return;
  }
  static volatile int started = 0;
  if (__sync_bool_compare_and_swap(&started, 0, 1)) {
    pthread_t thread;
    pthread_create(&thread, 0, printer, 0);
    pthread_detach(thread);
  }
  // Wall time, including threads blocked in a lock or a syscall.
  // CPU-time timers hide the waits that inflate the phase.
  struct sigevent sev;
  memset(&sev, 0, sizeof sev);
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = SIGPROF;
  sev._sigev_un._tid = static_cast<int>(syscall(SYS_gettid));
  timer_t timer;
  if (timer_create(CLOCK_MONOTONIC, &sev, &timer) != 0) {
    return;
  }
  armedThreads.fetch_add(1, std::memory_order_relaxed);
  struct itimerspec spec;
  memset(&spec, 0, sizeof spec);
  spec.it_interval.tv_nsec = 1000000;
  spec.it_value.tv_nsec = 1000000;
  timer_settime(timer, 0, &spec, 0);
}
