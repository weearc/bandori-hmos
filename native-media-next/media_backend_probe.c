#define _GNU_SOURCE 1
#include "media_backend_probe.h"
#include "media_extractor.h"
#include "media_format.h"
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

static void le16(unsigned char *p,uint16_t n){p[0]=n;p[1]=n>>8;}
static void le32(unsigned char *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(n>>(8*i));}
static int write_all(int fd,const void *buffer,size_t size){
 const unsigned char *p=buffer;
 while(size){ssize_t n=write(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=n;}return 0;
}
/* Real deterministic PCM container; never played. The backend must parse and
 * demux this file through OH_AVSource/OH_AVDemuxer, not return fixture bytes. */
static int create_wave(const char *path){
 unsigned char header[44]={0},block[4096];int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY,0600);if(fd<0)return -1;
 memcpy(header,"RIFF",4);le32(header+4,192036);memcpy(header+8,"WAVEfmt ",8);
 le32(header+16,16);le16(header+20,1);le16(header+22,2);le32(header+24,48000);
 le32(header+28,192000);le16(header+32,4);le16(header+34,16);memcpy(header+36,"data",4);le32(header+40,192000);
 int rc=write_all(fd,header,sizeof header);
 for(unsigned frame=0;!rc&&frame<48000;){
  unsigned count=48000-frame;if(count>1024)count=1024;
  for(unsigned i=0;i<count;++i){int sample=(int)((frame+i)%257)-128;le16(block+4*i,(uint16_t)sample);le16(block+4*i+2,(uint16_t)-sample);}
  rc=write_all(fd,block,count*4);frame+=count;
 }
 if(close(fd)!=0)rc=-1;return rc;
}

int media_backend_probe_run(const char *directory){
 char path[2048];void *ex=NULL,*format=NULL,*typed=NULL,*audio=NULL;unsigned char *a=NULL,*b=NULL;
 OH_AVFormat *audio_copy=NULL;
 int fd=-1,rc=0;int32_t channels=0,rate=0;int64_t first_pts=-1,seek_pts=-1;ssize_t bytes=-1;size_t tracks=0;
 const size_t capacity=262144;
 size_t total_pcm=0,sample_count=0;bool eos_checked=false;
#define MEDIA_CHECK(expr,code) do{if(!(expr)){rc=code;goto finish;}}while(0)
 MEDIA_CHECK(directory&&snprintf(path,sizeof path,"%s/native-media-fixture.wav",directory)<(int)sizeof path,1);
 MEDIA_CHECK(create_wave(path)==0,2);
 a=malloc(capacity);b=malloc(capacity);MEDIA_CHECK(a&&b,3);
 fd=open(path,O_RDONLY);MEDIA_CHECK(fd>=0,4);
 ex=android_media_extractor_new();MEDIA_CHECK(ex,5);
 MEDIA_CHECK(android_media_extractor_set_data_source_fd(ex,-1,0,192044)<0,30);
 MEDIA_CHECK(android_media_extractor_set_data_source_fd(ex,fd,0,192044)==0,6);
 close(fd);fd=-1; /* The extractor must own its duplicate, not this caller FD. */
 tracks=android_media_extractor_get_track_count(ex);MEDIA_CHECK(tracks==1,7);
 format=android_media_extractor_get_track_format(ex,0);MEDIA_CHECK(format,8);
 MEDIA_CHECK(android_media_format_get_int32(format,"channel-count",&channels)&&channels==2,9);
 MEDIA_CHECK(android_media_format_get_int32(format,"sample-rate",&rate)&&rate==48000,10);
 MEDIA_CHECK(android_media_extractor_select_track(ex,0)==0,11);
 MEDIA_CHECK(android_media_extractor_get_sample_track_index(ex)==0,12);
 first_pts=android_media_extractor_get_sample_time(ex);MEDIA_CHECK(first_pts>=0,13);
 bytes=android_media_extractor_read_sample_data(ex,a,capacity);MEDIA_CHECK(bytes>0&&(size_t)bytes<=capacity&&bytes%4==0,14);
 MEDIA_CHECK(android_media_extractor_read_sample_data(ex,b,capacity)==bytes&&memcmp(a,b,(size_t)bytes)==0,15);
 MEDIA_CHECK(android_media_extractor_get_sample_time(ex)==first_pts,16);
 for(size_t i=0;i<(size_t)bytes&&i<4096;i+=4){
  unsigned char expected[4];int sample=(int)((i/4)%257)-128;le16(expected,(uint16_t)sample);le16(expected+2,(uint16_t)-sample);
  MEDIA_CHECK(memcmp(a+i,expected,4)==0,17);
 }
 (void)android_media_extractor_advance(ex);
 MEDIA_CHECK(android_media_extractor_seek_to(ex,500000,2)==0,18);
 seek_pts=android_media_extractor_get_sample_time(ex);MEDIA_CHECK(seek_pts>=0&&seek_pts<1000000,19);
 MEDIA_CHECK(android_media_extractor_read_sample_data(ex,b,capacity)>0,20);
 MEDIA_CHECK(android_media_extractor_seek_to(ex,0,0)==0,21);
 MEDIA_CHECK(android_media_extractor_read_sample_data(ex,b,capacity)==bytes&&memcmp(a,b,(size_t)bytes)==0,22);
 for(sample_count=0;sample_count<512;){
  ssize_t n=android_media_extractor_read_sample_data(ex,b,capacity);MEDIA_CHECK(n>0,31);
  total_pcm+=(size_t)n;++sample_count;MEDIA_CHECK(total_pcm<=192000,32);
  if(!android_media_extractor_advance(ex))break;
 }
 MEDIA_CHECK(total_pcm==192000&&strstr(android_media_extractor_last_error(ex),"OH EOS flag")!=NULL,33);
 MEDIA_CHECK(android_media_extractor_get_sample_time(ex)==-1&&android_media_extractor_get_sample_track_index(ex)==-1&&android_media_extractor_read_sample_data(ex,b,capacity)==-1,34);
 eos_checked=true;
 MEDIA_CHECK(android_media_extractor_seek_to(ex,0,0)==0&&android_media_extractor_read_sample_data(ex,b,capacity)==bytes&&memcmp(a,b,(size_t)bytes)==0,35);
 MEDIA_CHECK(android_media_extractor_seek_to(ex,0,99)<0,23);
 MEDIA_CHECK(android_media_extractor_set_data_source(ex,"https://example.invalid/media")==ANDROID_MEDIA_ERROR_UNSUPPORTED,24);
 void *old_format=format;MEDIA_CHECK(android_media_format_delete(format)==0,25);format=NULL;
 MEDIA_CHECK(!android_media_format_get_int32(old_format,"sample-rate",&rate),26);
 OH_AVFormat *native=OH_AVFormat_Create();MEDIA_CHECK(native,36);
 bool populated=OH_AVFormat_SetDoubleValue(native,OH_MD_KEY_FRAME_RATE,29.97)&&OH_AVFormat_SetIntValue(native,OH_MD_KEY_PIXEL_FORMAT,2)&&OH_AVFormat_SetIntValue(native,OH_MD_KEY_VIDEO_STRIDE,128);
 typed=media_format_wrap(native);MEDIA_CHECK(populated&&typed,37);
 float fps=0;int32_t value=0;
 MEDIA_CHECK(android_media_format_get_float(typed,"frame-rate",&fps)&&fps>29.96f&&fps<29.98f,38);
 MEDIA_CHECK(!android_media_format_get_int32(typed,"color-format",&value)&&!android_media_format_get_int32(typed,"stride",&value),39);
 android_media_format_set_int32(typed,"color-format",21);android_media_format_set_int32(typed,"stride",320);
 MEDIA_CHECK(android_media_format_get_int32(typed,"color-format",&value)&&value==21&&android_media_format_get_int32(typed,"stride",&value)&&value==320,40);
 MEDIA_CHECK(!android_media_format_get_int32(ex,"sample-rate",&value)&&android_media_extractor_get_track_count(typed)==0,41);
 MEDIA_CHECK(media_format_clone_for_audio(typed)==NULL,42);
 OH_AVFormat *audio_native=OH_AVFormat_CreateAudioFormat(OH_AVCODEC_MIMETYPE_AUDIO_AAC,48000,2);
 MEDIA_CHECK(audio_native,43);
 const unsigned char csd[]={0x11,0x90};
 populated=OH_AVFormat_SetBuffer(audio_native,OH_MD_KEY_CODEC_CONFIG,csd,sizeof csd);
 audio=media_format_wrap(audio_native);MEDIA_CHECK(populated&&audio,44);
 const int32_t android_pcm[]={2,3,4,21,22};
 const int32_t oh_pcm[]={SAMPLE_S16LE,SAMPLE_U8,SAMPLE_F32LE,SAMPLE_S24LE,SAMPLE_S32LE};
 for(size_t i=0;i<sizeof android_pcm/sizeof android_pcm[0];++i){
  android_media_format_set_int32(audio,"pcm-encoding",android_pcm[i]);
  MEDIA_CHECK(android_media_format_get_int32(audio,"pcm-encoding",&value)&&value==android_pcm[i],45);
  audio_copy=media_format_clone_for_audio(audio);MEDIA_CHECK(audio_copy,46);
  MEDIA_CHECK(OH_AVFormat_GetIntValue(audio_copy,OH_MD_KEY_AUDIO_SAMPLE_FORMAT,&value)&&value==oh_pcm[i],47);
  OH_AVFormat_Destroy(audio_copy);audio_copy=NULL;
 }
 android_media_format_set_int32(audio,"pcm-encoding",999);
 MEDIA_CHECK(!android_media_format_get_int32(audio,"pcm-encoding",&value)&&media_format_clone_for_audio(audio)==NULL,48);
 android_media_format_set_int32(audio,"pcm-encoding",2);
 audio_copy=media_format_clone_for_audio(audio);MEDIA_CHECK(audio_copy,49);
 MEDIA_CHECK(android_media_format_delete(audio)==0,50);audio=NULL;
 uint8_t *copied_csd=NULL;size_t copied_size=0;
 MEDIA_CHECK(OH_AVFormat_GetBuffer(audio_copy,OH_MD_KEY_CODEC_CONFIG,&copied_csd,&copied_size)&&copied_size==sizeof csd&&!memcmp(copied_csd,csd,sizeof csd),51);
finish:
 sdk_trace_fprintf(stderr,"NATIVE_MEDIA_BACKEND_RESULT %s check=%d backend=OH_AVSource+OH_AVDemuxer source=generated_pcm_wave tracks=%zu channels=%d rate=%d sample_bytes=%zd first_pts_us=%lld seek_request_us=500000 seek_pts_us=%lld total_pcm_bytes=%zu samples=%zu eos_checked=%d format_types=%d network=disabled single_track_only=1 engine_started=0 error=%s\n",
  rc?"FAIL":"PASS",rc,tracks,channels,rate,bytes,(long long)first_pts,(long long)seek_pts,total_pcm,sample_count,eos_checked,!rc,rc?(ex?android_media_extractor_last_error(ex):"extractor_not_created"):"none");
 if(typed)android_media_format_delete(typed);
 if(audio)android_media_format_delete(audio);
 if(audio_copy)OH_AVFormat_Destroy(audio_copy);
 if(format)android_media_format_delete(format);
 if(ex)android_media_extractor_delete(ex);
 if(fd>=0)close(fd);
 free(a);free(b);return rc;
#undef MEDIA_CHECK
}
