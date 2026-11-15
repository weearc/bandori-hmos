#include <stdio.h>
const char *sdk_trace_run(const char *);
int main(int argc,char **argv) {
 if(argc!=2){fprintf(stderr,"usage: sdktrace-runner /path/to/original-dex-dir\n");return 2;}
 puts(sdk_trace_run(argv[1]));return 0;
}
