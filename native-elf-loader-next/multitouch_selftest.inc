static bool mt_assert_get(struct dvm *vm,dvm_ref e,const char *name,const char *sig,int arg,int64_t expected){
 struct dvm_class *c=dvm_object_class(vm,e);struct dvm_method *m=c?dvm_find_method(vm,c,name,sig):NULL;
 union dvm_value a={.i=arg},r={0};
 if(!m||!dvm_call(vm,m,e,&a,m->arg_count,&r)||dvm_exception(vm))return false;
 return m->ret_kind=='J'?r.j==expected:m->ret_kind=='F'?fabsf(r.f-(float)expected)<.01f:r.i==expected;
}
static bool multitouch_selftest(struct dvm *vm){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/multitouch-selftest.txt","w");if(!f)return false;
 unsigned checks=0;bool pass=true;
#define MT_CHECK(name,expr) do{bool ok=(expr);checks++;fprintf(f,"%s %s\n",ok?"PASS":"FAIL",name);if(!ok){pass=false;goto end;}}while(0)
 mt_state s={0};mt_packet p={.size=sizeof p,.version=1,.count=1,.event_ns=10000000000ll,.action=0,.changed_id=70},q;
 p.points[0]=(mt_point){.host_id=70,.x=100,.y=900,.pressure=1};
 MT_CHECK("first_down",mt_normalize(&s,&p,&q)==0&&q.action==0&&q.count==1&&q.points[0].id==0);
 int64_t down=q.down_ns;
 p.count=2;p.changed_id=900;p.event_ns+=20000000;p.points[1]=(mt_point){.host_id=900,.x=300,.y=900,.pressure=1};
 MT_CHECK("second_down_under50ms",mt_normalize(&s,&p,&q)==0&&q.action==(5|(1<<8))&&q.count==2&&q.down_ns==down&&q.points[0].id==0&&q.points[1].id==1);
 p.count=3;p.changed_id=32;p.event_ns+=10000000;p.points[2]=(mt_point){.host_id=32,.x=500,.y=900,.pressure=1};
 MT_CHECK("third_down_independent",mt_normalize(&s,&p,&q)==0&&q.count==3&&q.action==(5|(2<<8))&&q.points[2].id==2);
 /* Reordered raw array must not reorder Android pointer indices/IDs. */
 mt_point temp=p.points[0];p.points[0]=p.points[2];p.points[2]=temp;p.action=2;p.event_ns+=10000000;
 for(unsigned i=0;i<3;i++){p.points[i].x+=10;p.points[i].y-=300;}
 MT_CHECK("move_all_three_stable_after_raw_reorder",mt_normalize(&s,&p,&q)==0&&q.action==2&&q.points[0].host_id==70&&q.points[1].host_id==900&&q.points[2].host_id==32&&q.points[0].y==600);
 dvm_ref e=sdk_motion_event_create_multi(vm,&q);MT_CHECK("construct_three_pointer_snapshot",e!=0&&!dvm_exception(vm));dvm_pin(vm,e);
 MT_CHECK("getPointerCount_three",mt_assert_get(vm,e,"getPointerCount","()I",0,3));
 MT_CHECK("getPointerId_index2",mt_assert_get(vm,e,"getPointerId","(I)I",2,2));
 MT_CHECK("findPointerIndex_missing",mt_assert_get(vm,e,"findPointerIndex","(I)I",19,-1));
 MT_CHECK("findPointerIndex_stable",mt_assert_get(vm,e,"findPointerIndex","(I)I",1,1));
 MT_CHECK("indexed_X",mt_assert_get(vm,e,"getX","(I)F",1,310));
 MT_CHECK("indexed_Y",mt_assert_get(vm,e,"getY","(I)F",2,600));
 MT_CHECK("original_eventTime",mt_assert_get(vm,e,"getEventTime","()J",0,p.event_ns/1000000));
 MT_CHECK("stream_downTime_unchanged",mt_assert_get(vm,e,"getDownTime","()J",0,down/1000000));
 MT_CHECK("original_nanosecond_timestamp",mt_assert_get(vm,e,"getEventTimeNano","()J",0,p.event_ns));
 struct dvm_class *c=dvm_object_class(vm,e);struct dvm_method *ob=dvm_find_method(vm,c,"obtain","(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;");union dvm_value a={.l=e},copy={0};
 MT_CHECK("obtain_copy",ob&&dvm_call(vm,ob,0,&a,1,&copy)&&copy.l&&!dvm_exception(vm));dvm_pin(vm,copy.l);
 MT_CHECK("copy_preserves_three_pointers",mt_assert_get(vm,copy.l,"getPointerCount","()I",0,3)&&mt_assert_get(vm,copy.l,"getX","(I)F",1,310));
 dvm_unpin(vm,copy.l);dvm_unpin(vm,e);
 p.action=1;p.changed_id=900;p.event_ns+=10000000;
 MT_CHECK("middle_UP_keeps_other_two",mt_normalize(&s,&p,&q)==0&&q.action==(6|(1<<8))&&q.count==3&&s.count==2&&s.points[0].id==0&&s.points[1].id==2);
 e=sdk_motion_event_create_multi(vm,&q);MT_CHECK("pointer_UP_snapshot",e!=0);dvm_pin(vm,e);
 MT_CHECK("actionMasked_pointerUP",mt_assert_get(vm,e,"getActionMasked","()I",0,6));
 MT_CHECK("actionIndex_middle",mt_assert_get(vm,e,"getActionIndex","()I",0,1));dvm_unpin(vm,e);
 p.changed_id=70;p.event_ns+=10000000;MT_CHECK("first_UP_keeps_third",mt_normalize(&s,&p,&q)==0&&q.action==6&&s.count==1&&s.points[0].id==2);
 p.changed_id=32;p.event_ns+=10000000;MT_CHECK("last_UP_ends_stream",mt_normalize(&s,&p,&q)==0&&q.action==1&&q.count==1&&q.points[0].id==2&&!s.count);
 MT_CHECK("orphan_UP_rejected_once",mt_normalize(&s,&p,&q)==5&&!s.count);
 p.count=1;p.points[0].host_id=70;p.changed_id=70;p.action=0;p.event_ns+=10000000;
 MT_CHECK("new_stream_reuses_free_ID",mt_normalize(&s,&p,&q)==0&&q.points[0].id==0);
 MT_CHECK("duplicate_DOWN_rejected",mt_normalize(&s,&p,&q)==3&&s.count==1);
 p.action=2;p.event_ns-=659000;MT_CHECK("submillisecond_host_regression_preserved_not_dropped",mt_normalize(&s,&p,&q)==0&&q.event_ns==p.event_ns&&q.down_ns==s.down_ns);
 p.event_ns+=2000000;p.action=3;MT_CHECK("CANCEL_clears_entire_stream",mt_normalize(&s,&p,&q)==0&&q.action==3&&!s.count);
end:
 fprintf(f,"pid=%d result=%s checks=%u scope=HAP_semantics_not_physical_gesture switches_default=0 moving=0\n",getpid(),pass?"PASS":"FAIL",checks);fclose(f);return pass;
#undef MT_CHECK
}
