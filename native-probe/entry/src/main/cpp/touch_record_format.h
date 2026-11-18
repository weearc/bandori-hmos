#pragma once
#include "touch_record_core.h"
#include <cstdio>
#include <cstddef>
namespace touchrec {
// Fixed-width integer/float %.9g text fits comfortably, including NaN/Inf.
constexpr size_t RowTextCapacity=4096;
inline int format_row(char* text,size_t capacity,const Row& r) {
 static_assert(Points==10,"CSV schema 1 has exactly ten point groups");
 return std::snprintf(text,capacity,
  "%llu,%d,%d,%.9g,%.9g,%.9g,%llu,%.9g,%.9g,%llu,%d"
  ",%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d"
  ",%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d"
  ",%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d"
  ",%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d"
  ",%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d"
  ",%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d"
  ",%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d"
  ",%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d"
  ",%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d"
  ",%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d"
  "\n",
  (unsigned long long)r.recv_ns,
  r.action,
  r.pointer_id,
  r.x,
  r.y,
  r.pressure,
  (unsigned long long)r.seq,
  r.screen_x,
  r.screen_y,
  (unsigned long long)r.event_ns,
  r.num_points,
  r.points[0].id,
  r.points[0].x,
  r.points[0].y,
  r.points[0].pressure,
  r.points[0].screen_x,
  r.points[0].screen_y,
  r.points[0].pressed,
  r.points[1].id,
  r.points[1].x,
  r.points[1].y,
  r.points[1].pressure,
  r.points[1].screen_x,
  r.points[1].screen_y,
  r.points[1].pressed,
  r.points[2].id,
  r.points[2].x,
  r.points[2].y,
  r.points[2].pressure,
  r.points[2].screen_x,
  r.points[2].screen_y,
  r.points[2].pressed,
  r.points[3].id,
  r.points[3].x,
  r.points[3].y,
  r.points[3].pressure,
  r.points[3].screen_x,
  r.points[3].screen_y,
  r.points[3].pressed,
  r.points[4].id,
  r.points[4].x,
  r.points[4].y,
  r.points[4].pressure,
  r.points[4].screen_x,
  r.points[4].screen_y,
  r.points[4].pressed,
  r.points[5].id,
  r.points[5].x,
  r.points[5].y,
  r.points[5].pressure,
  r.points[5].screen_x,
  r.points[5].screen_y,
  r.points[5].pressed,
  r.points[6].id,
  r.points[6].x,
  r.points[6].y,
  r.points[6].pressure,
  r.points[6].screen_x,
  r.points[6].screen_y,
  r.points[6].pressed,
  r.points[7].id,
  r.points[7].x,
  r.points[7].y,
  r.points[7].pressure,
  r.points[7].screen_x,
  r.points[7].screen_y,
  r.points[7].pressed,
  r.points[8].id,
  r.points[8].x,
  r.points[8].y,
  r.points[8].pressure,
  r.points[8].screen_x,
  r.points[8].screen_y,
  r.points[8].pressed,
  r.points[9].id,
  r.points[9].x,
  r.points[9].y,
  r.points[9].pressure,
  r.points[9].screen_x,
  r.points[9].screen_y,
  r.points[9].pressed);
}
struct FlushPolicy {
 size_t bytes=0;
 uint64_t last_ns=0;
 bool due(uint64_t now,bool high_frequency) const {
  return bytes && (high_frequency || bytes>=65536 || now-last_ns>=200000000ull);
 }
 void flushed(uint64_t now) {bytes=0;last_ns=now;}
};
}
