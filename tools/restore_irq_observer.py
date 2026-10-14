from pathlib import Path
import ast
script=Path(__file__).with_name('install_irq_observer.py')
edits=None
for node in ast.parse(script.read_text(encoding='utf-8-sig')).body:
    if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='changes' for t in node.targets):
        edits=ast.literal_eval(node.value)
root=Path.home()/'Documents/Arduino/libraries/M5Unit-NFC/src/unit'
plans=[]
for name, replacements in edits.items():
    p=root/name; b=p.with_suffix(p.suffix+'.ntag215.bak')
    current=p.read_bytes(); original=b.read_bytes()
    if current==original:
        print(name,'already restored'); continue
    nl='\r\n' if b'\r\n' in original else '\n'
    expected=original.decode().replace('\r\n','\n')
    for old,new in replacements: expected=expected.replace(old,new)
    if current!=expected.replace('\n',nl).encode():
        raise RuntimeError('Library has other edits; refusing overwrite: '+str(p))
    plans.append((p,original))
for p,original in plans:
    p.write_bytes(original)
    print(p.name,'restored byte-for-byte from backup')
