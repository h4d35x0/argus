import re, sys, glob, os
DRY = '--apply' not in sys.argv
SIZES = ['14','16','20','28']   # body text sizes -> Orbitron; 48 deferred
tot=0; files=0; kbskip=0; symskip=set()
for f in glob.glob('src/*.cpp'):
    if os.path.basename(f)=='matrix_bg.cpp': continue
    s=open(f,encoding='utf-8',errors='replace').read()
    # vars that ever hold an LV_SYMBOL glyph (keep Montserrat or the icon vanishes)
    symvars=set(re.findall(r'lv_label_set_text\w*\(\s*(\w+)\s*,\s*[^;]*LV_SYMBOL', s))
    # vars that are keyboards/textareas (LVGL widgets with symbol keys) - keep Montserrat
    kbvars=set(re.findall(r'(\w+)\s*=\s*lv_keyboard_create', s))
    orig=s
    def mk(sz):
        pat=re.compile(r'(set_style_text_font\(\s*(\w+)\s*,\s*&)lv_font_montserrat_'+sz+r'\b')
        def repl(m):
            var=m.group(2)
            if var in symvars: symskip.add(f"{os.path.basename(f)}:{var}"); return m.group(0)
            if var in kbvars:  return m.group(0)
            return m.group(1)+f'font_argus_label_{sz}'
        return pat,repl
    n_file=0
    for sz in SIZES:
        pat,repl=mk(sz); s,n=pat.subn(repl,s); n_file+=n
    # count real replacements (subn counts include no-op returns)
    real=len(re.findall(r'&font_argus_label_(?:14|16|28)\b',s))-len(re.findall(r'&font_argus_label_(?:14|16|28)\b',orig))
    if real>0:
        if '#include "theme.h"' not in s:
            hb=os.path.basename(f).replace('.cpp','.h')
            s=s.replace(f'#include "{hb}"', f'#include "{hb}"\n#include "theme.h"',1)
        tot+=real; files+=1; print(f"{real:3d}  {f}")
        if not DRY: open(f,'w',encoding='utf-8').write(s)
print(f"--- {'DRY ' if DRY else 'APPLIED '}{tot} new (14/16/28) across {files} files ---")
print(f"symbol labels protected: {len(symskip)}")
