/* Pure Android interpolator value objects. No frames, animation objects,
 * rendering, scheduling or SDK-specific behavior is provided here. */
#include <math.h>
struct math_interpolator {float factor;bool accelerate;};
static void math_interpolator_free(void *p){free(p);}
static bool math_interpolator_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 bool accelerate=!strcmp(c,"android/view/animation/AccelerateInterpolator");
 bool decelerate=!strcmp(c,"android/view/animation/DecelerateInterpolator");
 if(!accelerate&&!decelerate)return false;
 bool constructor=!strcmp(m,"<init>")&&((!strcmp(sig,"()V")&&n==0)||(!strcmp(sig,"(F)V")&&n==1));
 bool interpolate=!strcmp(m,"getInterpolation")&&!strcmp(sig,"(F)F")&&n==1;
 if(!constructor&&!interpolate)return false;
 memset(out,0,sizeof *out);struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","Interpolator receiver");return true;}
 if(constructor){
  if(o->data){dvm__throw(v,"java/lang/IllegalStateException","Interpolator already initialized");return true;}
  struct math_interpolator *p=malloc(sizeof *p);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","Interpolator");return true;}
  p->factor=n?a[0].f:1.0f;p->accelerate=accelerate;o->data=p;o->data_destroy=math_interpolator_free;return true;
 }
 if(o->data_destroy!=math_interpolator_free||!o->data){dvm__throw(v,"java/lang/IllegalStateException","Interpolator not initialized");return true;}
 struct math_interpolator *p=o->data;float x=a[0].f;
 if(p->accelerate)out->f=p->factor==1.0f?x*x:(float)pow((double)x,(double)(p->factor*2.0f));
 else out->f=p->factor==1.0f?1.0f-(1.0f-x)*(1.0f-x):1.0f-(float)pow((double)(1.0f-x),(double)(p->factor*2.0f));
 return true;
}
bool android_basics_interpolator_selftest(struct dvm *v,unsigned *checks){
 unsigned count=0;const char *classes[]={"android/view/animation/DecelerateInterpolator","android/view/animation/AccelerateInterpolator"};
 /* Exact binary values avoid tolerance hiding a wrong exponent or direction.
  * Include the original FragmentManager's factors 1.5 and 2.5. */
 const float factors[]={1.0f,1.5f,2.5f,0.0f};
 const float half_decelerate[]={0.75f,0.875f,0.96875f,0.0f};
 const float half_accelerate[]={0.25f,0.125f,0.03125f,1.0f};
 for(unsigned kind=0;kind<2;kind++)for(unsigned f=0;f<4;f++){
  struct dvm_class *cls=dvm_find_class_or_external(v,classes[kind]);dvm_ref ref=cls?dvm_new_object(v,cls):0;
  union dvm_value arg={.f=factors[f]},out={0};
  if(!ref||!android_basics_call(v,classes[kind],"<init>",f?"(F)V":"()V",ref,f?&arg:NULL,f?1:0,&out)||dvm_exception(v))goto fail;
  count++;arg.f=0.5f;
  if(!android_basics_call(v,classes[kind],"getInterpolation","(F)F",ref,&arg,1,&out)||dvm_exception(v)||out.f!=(kind?half_accelerate[f]:half_decelerate[f]))goto fail;
  count++;
  if(f<3){arg.f=0;if(!android_basics_call(v,classes[kind],"getInterpolation","(F)F",ref,&arg,1,&out)||dvm_exception(v)||out.f!=0)goto fail;count++;
   arg.f=1;if(!android_basics_call(v,classes[kind],"getInterpolation","(F)F",ref,&arg,1,&out)||dvm_exception(v)||out.f!=1)goto fail;count++;}
 }
 if(checks)*checks=count;sdk_trace_fprintf(stderr,"INTERPOLATOR_SELFTEST PASS checks=%u factor_default=1 factor_1_5=1 factor_2_5=1 factor_zero=1 endpoints=1 accelerate=1 decelerate=1 rendering=0\n",count);return true;
fail:
 if(checks)*checks=count;sdk_trace_fprintf(stderr,"INTERPOLATOR_SELFTEST FAIL checks=%u exception=%u\n",count,dvm_exception(v));return false;
}
