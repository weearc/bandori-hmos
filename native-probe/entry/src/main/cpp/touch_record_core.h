#pragma once
#include <atomic>
#include <cstdint>
namespace touchrec {
constexpr unsigned Points=10, Capacity=4096;
struct Point { int32_t id=-1,pressed=0; float x=0,y=0,pressure=0,screen_x=0,screen_y=0; };
struct Row {
 uint64_t recv_ns=0,seq=0,event_ns=0;
 int32_t action=0,pointer_id=0,num_points=0;
 float x=0,y=0,pressure=0,screen_x=0,screen_y=0;
 Point points[Points];
};
// One consumer. XComponent is normally serialized; reject concurrent producers
// explicitly rather than blocking the input thread or corrupting the recording.
struct Ring {
 std::atomic<bool> enabled{false};
 std::atomic<unsigned> writers{0},head{0},tail{0};
 std::atomic<uint64_t> lost{0};
 std::atomic<uint64_t> epoch{0};
 std::atomic_flag producer=ATOMIC_FLAG_INIT;
 Row rows[Capacity];
 bool push(const Row& row) {
  if(!enabled.load(std::memory_order_acquire))return false;
  uint64_t generation=epoch.load(std::memory_order_acquire);
  writers.fetch_add(1,std::memory_order_acq_rel);
  if(!enabled.load(std::memory_order_acquire)||generation!=epoch.load(std::memory_order_acquire)){writers.fetch_sub(1,std::memory_order_release);return false;}
  if(producer.test_and_set(std::memory_order_acquire)){lost.fetch_add(1);writers.fetch_sub(1,std::memory_order_release);return false;}
  unsigned t=tail.load(std::memory_order_relaxed),next=(t+1)%Capacity;
  bool ok=next!=head.load(std::memory_order_acquire);
  if(ok){rows[t]=row;tail.store(next,std::memory_order_release);}else lost.fetch_add(1);
  producer.clear(std::memory_order_release);writers.fetch_sub(1,std::memory_order_release);return ok;
 }
 bool pop(Row& row) {
  unsigned h=head.load(std::memory_order_relaxed);
  if(h==tail.load(std::memory_order_acquire))return false;
  row=rows[h];head.store((h+1)%Capacity,std::memory_order_release);return true;
 }
};
}
