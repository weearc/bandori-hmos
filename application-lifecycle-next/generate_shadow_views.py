"""Extract only original layout id/class declarations, never geometry or rendering."""
from pathlib import Path
import struct,zipfile,re,json,hashlib,importlib.util
root=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('res',root/'resource-table-next/gen_apk_resource_table.py')
res=importlib.util.module_from_spec(spec);spec.loader.exec_module(res)
table=(root/'sdk-init-next/apk_resource_table.inc').read_text(encoding='utf-8')
layouts={name:int(key,16) for key,name in re.findall(r'\{(0x[0-9a-f]+),"layout","([^"]+)"\}',table)}
apk=root/'native-probe/entry/src/main/resources/rawfile/sdkdex/base.apk'
rows=[];includes=[];roots={};trees={}
def view_class(tag):
    if tag in ('include','merge','fragment'):return None
    return (tag if '.' in tag else ('android.webkit.WebView' if tag=='WebView' else 'android.view.View' if tag=='View' else 'android.widget.'+tag)).replace('.','/')
with zipfile.ZipFile(apk) as z:
    pool,resources=res.parse_arsc(z.read('resources.arsc'))
    for lid,record in sorted(resources.items()):
        if record['type']!='layout' or record['complex'] or record['data_type']!=3:continue
        path=pool[record['data']]
        data=z.read(path);pos=8;strings=[];first=True;stack=[];nodes=[]
        while pos<len(data):
            kind,header,size=struct.unpack_from('<HHI',data,pos)
            assert size>=header and pos+size<=len(data)
            if kind==1:strings=res.parse_string_pool(data,pos,pos+size)
            if kind==0x102:
                ext=pos+16;name=struct.unpack_from('<I',data,ext+4)[0]
                start,step,count=struct.unpack_from('<HHH',data,ext+8)
                tag=strings[name];ident=0;included=0
                for i in range(count):
                    a=ext+start+i*step;attr=struct.unpack_from('<I',data,a+4)[0]
                    typ=data[a+15];value=struct.unpack_from('<I',data,a+16)[0]
                    if strings[attr]=='id' and typ==1:ident=value
                    if tag=='include' and strings[attr]=='layout' and typ==1:included=value
                    if tag=='view' and strings[attr]=='class' and typ==3:tag=strings[value]
                cls=view_class(tag)
                if first:roots[lid]=(ident,cls);first=False
                if ident and cls:rows.append((lid,ident,cls))
                if included:includes.append((lid,included,ident))
                node={'id':ident,'class':cls,'include':included,'children':[]}
                if stack:stack[-1]['children'].append(node)
                else:nodes.append(node)
                stack.append(node)
            if kind==0x103:
                assert stack,'unbalanced layout end tag'
                stack.pop()
            pos+=size
        assert not stack,'unbalanced layout start tag'
        trees[lid]=nodes
rows=set(rows)
for _ in range(16):
    added=set()
    for outer,inner,override in includes:
        root_id,root_class=roots.get(inner,(0,None))
        # Android applies an include's ID to the included root, not a wrapper.
        # A merge layout has no root View, so its include ID creates no View.
        added.update((outer,ident,cls) for layout,ident,cls in rows
                     if layout==inner and not (override and root_class and ident==root_id))
        if override and root_class:added.add((outer,override,root_class))
    if added.issubset(rows):break
    rows|=added
else:raise ValueError('layout include expansion did not converge')
rows=sorted(rows)
out=['/* Original APK view declarations only; no inflation/rendering. */','struct shadow_view_decl {unsigned layout,id;const char *klass;};','static const struct shadow_view_decl shadow_view_decls[]={']
out += ['{%du,%du,%s},'%(l,i,json.dumps(c)) for l,i,c in rows];out+=['};']
# Explicit resource policy: the C facade reads IDs derived from these original
# APK names. Captcha and the generic agreement/web-container prerequisites only.
# All attributes other than class/id and parent links remain deliberately ignored.
policy_names=('gsc_activity_captcha','gsc_common_error','gsc_activity_phone_login',
              'gsc_activity_announcement',
              'gsc_layout_apple','gsc_layout_account','gsc_layout_visitor','gsc_layout_wechat',
              'activity_base_webview_layout','gsc_common_web_view')
policies=[(layouts[name],name) for name in policy_names]
out+=['struct shadow_layout_policy {unsigned layout;const char *name;};',
      'static const struct shadow_layout_policy shadow_layout_policies[]={']
out+=['{%du,%s},'%(lid,json.dumps(name)) for lid,name in policies];out+=['};']
out+=['struct shadow_layout_node {unsigned layout,index;int parent;unsigned id;const char *klass;};',
      'static const struct shadow_layout_node shadow_layout_nodes[]={']
tree_rows=[]
def expand(lid,node,parent,chain,override=0):
    if node['include']:
        inner=node['include']
        if inner in chain:raise ValueError('cyclic layout include')
        for child in trees[inner]:expand(lid,child,parent,chain+(inner,),node['id'])
        return
    own_parent=parent
    if node['class']:
        index=sum(row[0]==lid for row in tree_rows)
        tree_rows.append((lid,index,parent,override or node['id'],node['class']))
        own_parent=index
    for child in node['children']:expand(lid,child,own_parent,chain)
for lid,name in policies:
    for node in trees[lid]:expand(lid,node,-1,(lid,))
out+=['{%du,%du,%d,%du,%s},'%(l,i,p,ident,json.dumps(c)) for l,i,p,ident,c in tree_rows]
out+=['};']
(root/'application-lifecycle-next/shadow_view_declarations.inc').write_text('\n'.join(out)+'\n',encoding='utf-8')
print('SHADOW_VIEW_DECLARATIONS',len(rows),'apk_sha256',hashlib.sha256(apk.read_bytes()).hexdigest())
print('SHADOW_LAYOUT_POLICY',policies,'nodes',len(tree_rows))
