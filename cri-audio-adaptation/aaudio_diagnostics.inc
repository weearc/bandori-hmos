// Executed only by the existing stream control worker, never the write callback.
static int64_t interval_percentile(Stream*s,unsigned pct){uint64_t n=0,counts[Stream::HIST_BINS];for(unsigned i=0;i<Stream::HIST_BINS;i++){counts[i]=s->intervals[i].load(std::memory_order_relaxed);n+=counts[i];}if(!n)return 0;uint64_t target=(n*pct+99)/100,total=0;for(unsigned i=0;i<Stream::HIST_BINS;i++){total+=counts[i];if(total>=target)return i==Stream::HIST_BINS-1?-1:(int64_t)((i+1)*Stream::HIST_QUANTUM);}return -1;}
static void audio_diagnostics(Stream*s){
#ifndef AAUDIO_OFFLINE_TEST
 if(in_callback==s)return;
 diagnosticsQuery=true;int burst=AAudioStream_getFramesPerBurst(s),buffer=AAudioStream_getBufferSizeInFrames(s);diagnosticsQuery=false;uint32_t xruns=0;int xr=-1;int64_t written=0;int wr=-1;
 {std::lock_guard<std::mutex>g(s->control);if(!s->closing&&s->renderer){xr=OH_AudioRenderer_GetUnderflowCount(s->renderer,&xruns);wr=OH_AudioRenderer_GetFramesWritten(s->renderer,&written);}}
 const char*source=s->burstSource==1?"OH_GetFrameSizeInCallback":s->burstSource==2?"OH_GetBufferSizeInFrames":s->burstSource==3?"observed_callback_frames":"unavailable";
#ifdef AAUDIO_LATENCY_BASELINE
 source="baseline_UNAVAILABLE";
#endif
 char line[2048];int n=snprintf(line,sizeof line,"pid=%d mono_ns=%llu stream=%p latency_mode=%s frames_per_burst=%d burst_source=%s buffer_frames=%d buffer_reason=%s buffer_get_symbol=%d buffer_set_symbol=%d sample_rate=%d channels=%d callbacks=%llu intervals=%llu interval_p50_ns_le=%lld interval_p99_ns_le=%lld interval_max_ns=%llu histogram_quantum_ns=%llu histogram_overflow=%llu xruns_rc=%d xruns=%u frames_written_rc=%d frames_written=%lld buffer_get_calls=%llu burst_get_calls=%llu buffer_set_calls=%llu last_set=%d oh_size_rc=%d oh_set_rc=%d\n",getpid(),(unsigned long long)audio_now(),(void*)s,s->performance==12?"FAST":"NORMAL",burst,source,buffer,s->sizeApi.buffer?"backend_result":s->sizeApi.bufferSymbol?"sdk_ABI_undeclared":"symbol_absent",s->sizeApi.bufferSymbol,s->sizeApi.setSymbol,s->rate,s->channels,(unsigned long long)s->writeCount.load(),(unsigned long long)s->intervalCount.load(),(long long)interval_percentile(s,50),(long long)interval_percentile(s,99),(unsigned long long)s->intervalMaxNs.load(),(unsigned long long)Stream::HIST_QUANTUM,(unsigned long long)s->intervals[Stream::HIST_BINS-1].load(),xr,xruns,wr,(long long)written,(unsigned long long)s->sizeGets.load(),(unsigned long long)s->burstGets.load(),(unsigned long long)s->sizeSets.load(),s->lastSet.load(),s->sizeRaw.load(),s->setRaw.load());
 static std::mutex logLock;std::lock_guard<std::mutex>g(logLock);const char*p="/data/storage/el2/base/haps/entry/files/sdkdex/audio-latency-status.txt";struct stat st{};
 if(!stat(p,&st)&&st.st_size>1024*1024-4096){std::string backup=std::string(p)+".1";unlink(backup.c_str());rename(p,backup.c_str());}
 int fd=open(p,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0644);if(fd>=0){fchmod(fd,0644);if(n>0)write(fd,line,std::min<int>(n,sizeof(line)-1));close(fd);}
#else
 (void)s;
#endif
}
