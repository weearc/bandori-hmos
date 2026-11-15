from pathlib import Path

p = Path('sdk-runtime-spike/build_tls_candidate.py')
s = p.read_text(encoding='utf-8')
anchor = "text = '#include \"security_provider.h\"\\n#include \"system_properties.h\"\\n#include \"cert_runtime.h\"\\n#include \"apk_signature_runtime.h\"\\n#include \"exception_hierarchy.h\"\\n#include \"ssl_runtime.h\"\\n' + text"
assert anchor in s

helper = r'''text = text.replace('static size_t trace_used;',
    'static size_t trace_used;\n'
    'static char trace_progress_path[1024];\n'
    'static void trace_progress_write(const char *s){\n'
    ' if(!trace_progress_path[0]||!s)return;\n'
    ' static const char *const prefixes[]={"BEGIN","END ","APPLICATION_","APPLICATION ","BUDGET_STAGE","SELFTEST","DISPATCH_SELFTEST","REENTRANT","LOCALE","INTENT_FILTER","THREAD_PRIORITY","EXECUTOR","THREAD_SHUTDOWN","MISSING_","EXCEPTION","RESULT","SDK_LOGIN","DEX_FILES","BLOCKED","JAVA_STATIC com/gsc","WEAK_CURRENT"};\n'
    ' bool keep=false;for(size_t i=0;i<sizeof prefixes/sizeof prefixes[0];++i)if(!strncmp(s,prefixes[i],strlen(prefixes[i]))){keep=true;break;}\n'
    ' if(!keep)return;FILE *f=fopen(trace_progress_path,"a");if(!f)return;fputs(s,f);fputc(10,f);fclose(f);\n'
    '}\n', 1)
assert text.count('static char trace_progress_path[1024];') >= 1
text = text.replace('int n=vsnprintf(trace+trace_used,sizeof(trace)-trace_used,fmt,a); va_end(a);',
    'char *trace_start=trace+trace_used;int n=vsnprintf(trace_start,sizeof(trace)-trace_used,fmt,a); va_end(a);trace_progress_write(trace_start);', 1)
text = text.replace('__attribute__((visibility("default"))) const char *sdk_trace_run(const char *dir) {',
    '__attribute__((visibility("default"))) const char *sdk_trace_run(const char *dir) {\n if(dir){snprintf(trace_progress_path,sizeof trace_progress_path,"%s/login-progress.txt",dir);FILE *pf=fopen(trace_progress_path,"w");if(pf)fclose(pf);}', 1)
'''
s = s.replace(anchor, helper + anchor, 1)
p.write_text(s, encoding='utf-8')
print('progress patch inserted')
