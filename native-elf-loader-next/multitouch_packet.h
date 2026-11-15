#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#define MT_MAX 16
/* Host snapshot ABI, v1. No VM references: safe to copy across threads. */
typedef struct { int32_t host_id,id,pressed; float x,y,raw_x,raw_y,pressure,size; } mt_point;
typedef struct {
 uint32_t size,version,count,host_count;
 uint64_t sequence,callback_ns,enqueue_ns;
 int64_t event_ns,down_ns;
 int32_t action,host_action,changed_id;
 mt_point points[MT_MAX];
} mt_packet;
typedef struct { unsigned count; int64_t down_ns,last_ns; mt_point points[MT_MAX]; } mt_state;
/* Preserve active order; Android pointer index is not pointer ID. UP snapshots
 * include the departing pointer; only then does it leave the active set. */
static int mt_normalize(mt_state *s,const mt_packet *host,mt_packet *out){
 if(!s||!host||!out||host->size!=sizeof *host||host->version!=1||host->count>MT_MAX||host->event_ns<0||host->action<0||host->action>3)return 1;
 for(unsigned i=0;i<host->count;i++){
  const mt_point *p=&host->points[i];
  if(!isfinite(p->x)||!isfinite(p->y)||p->host_id<0)return 1;
  for(unsigned j=0;j<i;j++)if(p->host_id==host->points[j].host_id)return 1;
 }
 /* Host DOWN can carry microseconds while the following MOVE is truncated
  * to milliseconds. Preserve both the original timestamp and arrival order;
  * the caller counts regressions, but must not discard a valid MOVE. */
 mt_state next=*s;int changed=-1,source=-1;
 for(unsigned i=0;i<next.count;i++){
  if(next.points[i].host_id==host->changed_id)changed=(int)i;
  for(unsigned j=0;j<host->count;j++)if(next.points[i].host_id==host->points[j].host_id){int id=next.points[i].id;next.points[i]=host->points[j];next.points[i].id=id;break;}
 }
 for(unsigned j=0;j<host->count;j++)if(host->points[j].host_id==host->changed_id)source=(int)j;
 int action=host->action;
 if(action==0){
  if(changed>=0)return 3; /* duplicate DOWN, not a MOVE-count error */
  if(source<0||next.count==MT_MAX)return 4;
  unsigned id=0;for(;id<32;id++){unsigned i=0;for(;i<next.count&&next.points[i].id!=(int)id;i++);if(i==next.count)break;}
  if(id==32)return 4;
  changed=(int)next.count;next.points[next.count]=host->points[source];next.points[next.count++].id=(int)id;
  if(next.count==1)next.down_ns=host->event_ns;
  action=next.count==1?0:5|(changed<<8);
 }else if(action==1){
  if(changed<0)return 5;
  action=next.count==1?1:6|(changed<<8);
 }else if(!next.count)return 6;
 *out=*host;out->host_count=host->count;out->host_action=host->action;
 out->count=next.count;out->down_ns=next.down_ns;out->action=action;
 memcpy(out->points,next.points,next.count*sizeof(mt_point));
 if(host->action==1){for(unsigned i=(unsigned)changed+1;i<next.count;i++)next.points[i-1]=next.points[i];next.count--;}
 if(host->action==3)next.count=0;
 if(!next.count)next.down_ns=0;
 next.last_ns=host->event_ns;*s=next;return 0;
}
