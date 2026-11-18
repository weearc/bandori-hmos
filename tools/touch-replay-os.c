// Standalone shell process -> public host OS input API. Never links guest/runtime.
#define _POSIX_C_SOURCE 200809L
typedef struct OH_PixelmapNative OH_PixelmapNative;
#include <multimodalinput/oh_input_manager.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
typedef struct {uint64_t rel,seq;int action,id,x,y;unsigned row;} Event;
static volatile sig_atomic_t stop;
static void interrupted(int sig){(void)sig;stop=1;}
static uint64_t mono(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static int inject(Input_TouchEvent* e,const Event* r){
 static const int action[]={TOUCH_ACTION_DOWN,TOUCH_ACTION_UP,TOUCH_ACTION_MOVE,TOUCH_ACTION_CANCEL};
 OH_Input_SetTouchEventAction(e,action[r->action]);OH_Input_SetTouchEventFingerId(e,r->id);
 OH_Input_SetTouchEventDisplayX(e,r->x);OH_Input_SetTouchEventDisplayY(e,r->y);
 OH_Input_SetTouchEventActionTime(e,(int64_t)(mono()/1000));
 return OH_Input_InjectTouchEvent(e);
}
int main(int argc,char** argv){
 if(argc==2&&!strcmp(argv[1],"--probe")){
  Input_InjectionStatus s=UNAUTHORIZED;int rc=OH_Input_QueryAuthorizedStatus(&s);
  printf("query_rc=%d authorized_status=%d input_events_sent=0\n",rc,s);return 0;
 }
 if(argc!=5){fprintf(stderr,"usage: touch-replay-os PLAN.csv OUTPUT.csv SPEED LEAD_MS | --probe\n");return 2;}
 char* end;double speed=strtod(argv[3],&end);if(*end||!isfinite(speed)||speed<=0||speed>8)return 2;
 long lead=strtol(argv[4],&end,10);if(*end||lead<0||lead>600000)return 2;
 FILE* f=fopen(argv[1],"r");if(!f){perror("plan");return 2;}
 // Validate the entire plan before sending any OS event.
 Event* rows=calloc(1000000,sizeof *rows);if(!rows)return 2;
 char line[512];unsigned n=0;fgets(line,sizeof line,f);
 while(fgets(line,sizeof line,f)){
  unsigned long long rel,seq;Event r;char extra;
  if(n==1000000||sscanf(line,"%llu,%d,%d,%d,%d,%llu,%u %c",&rel,&r.action,&r.id,&r.x,&r.y,&seq,&r.row,&extra)!=7||r.action<0||r.action>3||r.id<0||r.id>=10||r.x<0||r.y<0||(n&&rel<rows[n-1].rel)){fprintf(stderr,"invalid plan row %u\n",n);return 2;}
  r.rel=rel;r.seq=seq;rows[n++]=r;
 }
 fclose(f);if(!n)return 2;
 f=fopen(argv[2],"wx");if(!f){perror("output");return 2;}if(fchmod(fileno(f),0644))return 2;
 // Preallocate all result storage. No diagnostic file IO in the send loop.
 uint64_t* sent=calloc(n,sizeof *sent);uint64_t* returned=calloc(n,sizeof *returned);int* codes=calloc(n,sizeof *codes);
 if(!sent||!returned||!codes)return 2;
 Input_TouchEvent* e=OH_Input_CreateTouchEvent();if(!e)return 2;
 signal(SIGTERM,interrupted);signal(SIGINT,interrupted);
 uint64_t start=mono()+(uint64_t)lead*1000000;unsigned issued=0,accepted=0;Event active[10];int down[10]={0},error=0;
 for(unsigned i=0;i<n&&!stop;i++){
  uint64_t target=start+(uint64_t)(rows[i].rel/speed);struct timespec t={(time_t)(target/1000000000),(long)(target%1000000000)};
  int rc;while((rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL))==EINTR&&!stop){}
  if(stop)break;if(rc){error=rc;break;}
  sent[i]=mono();codes[i]=inject(e,&rows[i]);returned[i]=mono();issued++;
  if(codes[i]){error=codes[i];break;}
  accepted++;int id=rows[i].id;active[id]=rows[i];
  if(rows[i].action==0)down[id]=1;else if(rows[i].action==1)down[id]=0;else if(rows[i].action==3)memset(down,0,sizeof down);
 }
 unsigned cleanup=0;for(int id=0;id<10;id++)if(down[id]){active[id].action=1;inject(e,&active[id]);cleanup++;}
 OH_Input_DestroyTouchEvent(&e);
 fputs("index,source_row,seq,relative_ns,target_ns,send_ns,return_ns,rc,lateness_ns,action,pointer_id,x,y\n",f);
 for(unsigned i=0;i<issued;i++){uint64_t target=start+(uint64_t)(rows[i].rel/speed);fprintf(f,"%u,%u,%llu,%llu,%llu,%llu,%llu,%d,%lld,%d,%d,%d,%d\n",i,rows[i].row,(unsigned long long)rows[i].seq,(unsigned long long)rows[i].rel,(unsigned long long)target,(unsigned long long)sent[i],(unsigned long long)returned[i],codes[i],(long long)sent[i]-(long long)target,rows[i].action,rows[i].id,rows[i].x,rows[i].y);}
 int io=ferror(f);if(fclose(f))io=1;
 printf("planned=%u issued=%u os_accepted=%u cleanup_events=%u error=%d interrupted=%d io_error=%d\n",n,issued,accepted,cleanup,error,(int)stop,io);
 free(rows);free(sent);free(returned);free(codes);return error||stop||io||issued!=n?1:0;
}
