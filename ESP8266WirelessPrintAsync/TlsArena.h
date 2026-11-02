#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

// Every TLS session asks mbedtls for two buffers of about sixteen kilobytes, one for
// each direction, and asks for them as one unbroken piece. That is the whole of the
// memory trouble on this module: the heap has plenty of room in total, but while a
// print is running under web load its largest unbroken piece sits around nineteen
// kilobytes, and a handshake started in that state fails, leaves rubbish behind and
// takes the next one down with it.
//
// A pool taken from the heap was tried before and made matters worse, because a pool
// taken from the heap is itself a hole in the heap. These slots are not taken from the
// heap at all: they are plain arrays in .bss, so the heap never sees them and never has
// to find room for them. The core is built with CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC, which
// means MBEDTLS_PLATFORM_MEMORY, which means mbedtls_platform_set_calloc_free is in the
// library and the hook below is all it takes.
//
// Two slots, because exactly one session is ever open and a session wants exactly two.
// A third large request, if one ever appears, falls back to the heap rather than failing.

#define TLS_ARENA_SLOTS 2
#define TLS_ARENA_SLOT  17536           // 16384 of content plus record and MAC overhead
#define TLS_ARENA_FLOOR 8192            // Below this it is not a session buffer, leave it to the heap

static uint8_t tlsArenaSlot[TLS_ARENA_SLOTS][TLS_ARENA_SLOT] __attribute__((aligned(8)));
static bool tlsArenaTaken[TLS_ARENA_SLOTS];
static portMUX_TYPE tlsArenaLock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t tlsArenaServed, tlsArenaMissed, tlsArenaLive;
static size_t tlsArenaWidest;           // Largest request seen, so the slot size can be checked against reality

static void *tlsArenaCalloc(size_t count, size_t size) {
  const size_t want = count * size;
  if (size && want / size != count)     // A count that wraps must not become a short buffer
    return NULL;

  if (want >= TLS_ARENA_FLOOR) {
    if (want > tlsArenaWidest)
      tlsArenaWidest = want;

    if (want <= TLS_ARENA_SLOT) {
      int slot = -1;
      portENTER_CRITICAL(&tlsArenaLock);
      for (int i = 0; i < TLS_ARENA_SLOTS; ++i)
        if (!tlsArenaTaken[i]) {
          tlsArenaTaken[i] = true;
          slot = i;
          ++tlsArenaLive;
          break;
        }
      portEXIT_CRITICAL(&tlsArenaLock);

      if (slot >= 0) {
        ++tlsArenaServed;
        memset(tlsArenaSlot[slot], 0, want);
        return tlsArenaSlot[slot];
      }
    }
    ++tlsArenaMissed;
  }

  return heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void tlsArenaFree(void *block) {
  if (!block)
    return;

  for (int i = 0; i < TLS_ARENA_SLOTS; ++i)
    if (block == tlsArenaSlot[i]) {
      portENTER_CRITICAL(&tlsArenaLock);
      tlsArenaTaken[i] = false;
      if (tlsArenaLive)
        --tlsArenaLive;
      portEXIT_CRITICAL(&tlsArenaLock);
      return;
    }

  heap_caps_free(block);
}

inline void tlsArenaBegin() {
  mbedtls_platform_set_calloc_free(tlsArenaCalloc, tlsArenaFree);
}

// Whether the slots are actually carrying the session buffers. A widest figure above the
// slot size, or a missed count that climbs, means the sizing below is wrong for this core
// and the handshake has gone back to competing for the heap.
inline String tlsArenaReport() {
  return String(tlsArenaLive) + " of " + String(TLS_ARENA_SLOTS) + " slots in use, "
       + String(tlsArenaServed) + " served, " + String(tlsArenaMissed) + " missed, widest "
       + String(tlsArenaWidest) + " of " + String(TLS_ARENA_SLOT);
}
