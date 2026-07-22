import re, sys, glob, os
DRY = '--apply' not in sys.argv
# Migrate montserrat_20 body labels -> font_argus_label_20 (Orbitron), but ONLY for
# named label vars that never receive an LV_SYMBOL glyph (those must stay in the
# Montserrat symbol font or the icon vanishes). Generic reused locals (lbl/label/l/
# obj/o) are skipped as ambiguous. Titles already use font_argus_ui.
GENERIC = set()
fontpat = re.compile(r'(set_style_text_font\(\s*(\w+)\s*,\s*&)lv_font_montserrat_20\b')
tot=0; files=0; skipped_sym=set(); skipped_gen=set()
for f in glob.glob('src/*.cpp'):
    s=open(f,encoding='utf-8',errors='replace').read()
    # label vars that ever hold a symbol in THIS file
    symvars=set(re.findall(r'lv_label_set_text\w*\(\s*(\w+)\s*,\s*[^;]*LV_SYMBOL', s))
    def repl(m):
        var=m.group(2)
        if var in symvars: skipped_sym.add(f"{os.path.basename(f)}:{var}"); return m.group(0)
        if var in GENERIC: skipped_gen.add(f"{os.path.basename(f)}:{var}"); return m.group(0)
        return m.group(1)+'font_argus_label_20'
    new,n=fontpat.subn(repl,s)
    # count only real replacements
    real=len(re.findall(r'&font_argus_label_20',new))-len(re.findall(r'&font_argus_label_20',s))
    if real>0:
        if '#include "theme.h"' not in new:
            hb=os.path.basename(f).replace('.cpp','.h')
            new=new.replace(f'#include "{hb}"', f'#include "{hb}"\n#include "theme.h"',1)
        tot+=real; files+=1; print(f"{real:3d}  {f}")
        if not DRY: open(f,'w',encoding='utf-8').write(new)
print(f"--- {'DRY ' if DRY else 'APPLIED '}{tot} montserrat_20->Orbitron across {files} files ---")
print(f"skipped (symbol labels, kept Montserrat): {len(skipped_sym)}")
for x in sorted(skipped_sym): print("   sym:",x)
print(f"skipped (generic locals): {sorted(skipped_gen)}")
