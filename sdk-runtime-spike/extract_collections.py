"""Reproducibly extract reviewed MPL collection functions from pinned Lunaria."""
from pathlib import Path
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parent
NAMES = ['list_of', 'list_init', 'list_add', 'list_get', 'list_size', 'list_isEmpty',
         'list_clear', 'list_contains', 'list_remove', 'list_add_at', 'list_remove_obj',
         'list_toArray', 'list_set', 'map_key_eq', 'map_find_key', 'map_destroy',
         'map_init', 'map_put', 'map_get', 'map_containsKey', 'map_erase_at',
         'map_remove', 'map_size', 'map_clear', 'map_isEmpty']

def generate():
    source = (ROOT.parent / 'research/lunaria-source/dvm_runtime.c').read_bytes()
    text = source.decode('utf-8')
    output = text[:text.index('*/') + 2] + '\n/* Extracted and adapted by extract_collections.py; see collections-provenance.json. */\n'
    for name in ['rt_list', 'rt_map']:
        output += re.search(r'^struct ' + name + r' \{.*?};', text, re.M | re.S).group() + '\n'
    output += '''
static bool list_contains(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static bool key_eq(struct dvm *v,dvm_ref a,dvm_ref b){return java_basics_equal(v,a,b);}
/* Sorted/enum map classes are not exposed by this module. Never substitute an
 * invented comparator if their internal paths are reached. */
static bool map_compare_key(struct dvm *v,dvm_ref s,dvm_ref a,dvm_ref b,int32_t *r){(void)s;(void)a;(void)b;(void)r;dvm__throw(v,"java/lang/UnsupportedOperationException","sorted maps not installed");return false;}
static bool tree_map_lower_bound(struct dvm *v,dvm_ref s,dvm_ref k,uint32_t *i,bool *e){(void)s;(void)k;(void)i;(void)e;dvm__throw(v,"java/lang/UnsupportedOperationException","sorted maps not installed");return false;}
'''
    records = []
    for name in NAMES:
        # Exclude forward declarations and lexically balance body braces. Some
        # upstream functions are one-line definitions; braces in comments and
        # string/character literals must not affect the balance.
        match = re.search(r'^static [^;\n]*?\b' + name + r'\([^;]*?\)\s*\{', text, re.M | re.S)
        if not match:
            raise ValueError('Missing upstream function ' + name)
        depth = 1
        end = None
        tokens = re.compile(r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]''', re.S)
        for token in tokens.finditer(text, match.end()):
            if token.group() == '{':
                depth += 1
            elif token.group() == '}':
                depth -= 1
                if not depth:
                    end = token.end()
                    break
        if end is None:
            raise ValueError('Unbalanced function ' + name)
        original = text[match.start():end]
        fn = original
        if name in ('list_remove', 'list_set'):
            fn = re.sub(r'if \(!l \|\| i < 0 \|\| \(uint32_t\)i >= l->size\) RETL\(0\);|if \(!l \|\| i >= l->size\) RETL\(0\);',
                        'if (!l || (uint32_t)i >= l->size) { dvm__throw(vm,"java/lang/IndexOutOfBoundsException","list index"); return false; }', fn)
        if name in ('list_init', 'map_init'):
            fn = fn.replace('   RETV();\n}', '   if (!o->data) { dvm__throw(vm,"java/lang/OutOfMemoryError","collection storage"); return false; }\n   RETV();\n}')
        if name in ('list_add', 'list_add_at'):
            fn = fn.replace('   if (l->size == l->cap) {', '   if (l->size >= 1000000) { dvm__throw(vm,"java/lang/OutOfMemoryError","diagnostic collection limit"); return false; }\n   if (l->size == l->cap) {')
        if name == 'list_add':
            fn = fn.replace('if (!l) RETI(0);', 'if (!l) { dvm__throw(vm,"java/lang/OutOfMemoryError","list storage"); return false; }')
            fn = fn.replace('if (!n) RETI(0);', 'if (!n) { dvm__throw(vm,"java/lang/OutOfMemoryError","list growth"); return false; }')
        if name == 'list_contains':
            fn = fn.replace('if (key_eq(vm, l->items[i], ARG(0).l)) RETI(1);', 'bool hit = key_eq(vm, ARG(0).l, l->items[i]);\n      if (dvm_exception(vm)) return false;\n      if (hit) RETI(1);')
        if name == 'list_remove_obj':
            fn = fn.replace('key_eq(vm, l->items[i], ARG(0).l)', 'key_eq(vm, ARG(0).l, l->items[i])')
            fn = fn.replace('      if (!hit) continue;', '      if (dvm_exception(vm)) return false;\n      if (!hit) continue;')
        if name == 'map_put':
            fn = fn.replace('if (!mp) RETL(0);', 'if (!mp) { dvm__throw(vm,"java/lang/OutOfMemoryError","map storage"); return false; }')
            fn = fn.replace('   if (mp->size == mp->cap) {', '   if (mp->size >= 1000000) { dvm__throw(vm,"java/lang/OutOfMemoryError","diagnostic collection limit"); return false; }\n   if (mp->size == mp->cap) {')
            fn = fn.replace('if (!n) RETL(0);', 'if (!n) { dvm__throw(vm,"java/lang/OutOfMemoryError","map growth"); return false; }')
        if name == 'list_init':
            fn = fn.replace('   if (o->data) ((struct rt_list *)o->data)->cap = 8;', '   o->data_destroy = list_gc_destroy;\n   if (o->data) ((struct rt_list *)o->data)->cap = 8;')
        output += '\n' + fn + '\n'
        records.append({'function': name, 'upstream_line': text.count('\n', 0, match.start()) + 1,
                        'original_function_sha256': hashlib.sha256(original.encode()).hexdigest()})
    (ROOT / 'generated/upstream_collections.inc').write_text(output, encoding='utf-8')
    doc = {'license': 'MPL-2.0', 'upstream_commit': '8ed4b75368153f2b9d1174c986e50c8fe87aee59',
           'source': 'research/lunaria-source/dvm_runtime.c', 'source_sha256': hashlib.sha256(source).hexdigest(),
           'functions': records,
           'adaptations': ['collection-only dispatch; no desktop framework runtime',
                           'strict bounds and allocation errors', 'one-million-element diagnostic limit',
                           'existing Java equality bridge with exception propagation',
                           'sorted map paths explicitly unsupported',
                           'local fail-fast iterator and constructor/bulk adapters in collections.c']}
    (ROOT / 'collections-provenance.json').write_text(json.dumps(doc, indent=2) + '\n')

if __name__ == '__main__':
    generate()
