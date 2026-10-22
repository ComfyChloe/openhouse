// The driver's room, published for tools that align the lighthouse frame to something
// else (alignment tools): POSIX shared memory /lhdrvrw-room-<uid> (Linux).
//
// Poses are in the host's raw tracking frame (what the base stations and devices are
// reported in: WorldFromDriver * pose). "settled" says the room has stopped moving: no
// base has moved for kSettleTime and every visible base is placed. "epoch" changes when
// a settled room differs from the last settled one (a base moved, or was added): what
// was measured against the old poses is stale. A reader takes the room only while
// "beat" is fresh (the driver writes it every 0.25 s) and the pid is alive.
//
// Plain C layout, shared with alignment tools (src/linux/lhdrvrw_room.h is a copy).
#pragma once
#include <stdint.h>

#define LHDRVRW_ROOM_MAGIC 0x4D52484Cu  // "LHRM"
#define LHDRVRW_ROOM_VERSION 1
#define LHDRVRW_ROOM_MAX_BASES 16

typedef struct {
  char serial[32];       // "LHB-XXXXXXXX"
  int32_t visible;       // seen this session
  int32_t placed;        // pose known
  int32_t origin;        // the room's origin base
  int32_t pad;
  double p[3], q[4];     // raw frame, q: w x y z
  double sigma;          // m, position uncertainty
} lhdrvrw_room_base;

typedef struct {
  uint32_t magic, version;
  uint32_t seq;          // seqlock: odd while writing (use atomics on both sides)
  int32_t pid;
  double beat;           // CLOCK_MONOTONIC s
  uint64_t epoch;        // 0: never settled
  int32_t settled;
  int32_t count;
  char room[64];
  lhdrvrw_room_base bases[LHDRVRW_ROOM_MAX_BASES];
} lhdrvrw_room;

#define LHDRVRW_ROOM_PATH_PREFIX "/lhdrvrw-room-"
