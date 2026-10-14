from pathlib import Path
import shutil
root=Path.home()/'Documents/Arduino/libraries/M5Unit-NFC/src/unit'
changes={
'unit_ST25R3916.hpp': [('    bool readInterrupts(uint32_t& value);', '''    bool readInterrupts(uint32_t& value);
    // Observer runs synchronously after an existing IRQ read; do not perform I/O here.
    using InterruptObserver = void (*)(uint32_t, void*);
    void setInterruptObserver(InterruptObserver callback, void* context = nullptr)
    {
        _interruptObserver = callback;
        _interruptObserverContext = context;
    }'''), ('    config_t _cfg{};', '''    config_t _cfg{};
    InterruptObserver _interruptObserver{};
    void* _interruptObserverContext{};''')],
'unit_ST25R3916.cpp': [('        value = ((uint32_t)main_nfc << 16) | ((uint32_t)error << 8) | ((uint32_t)passive);', '''        value = ((uint32_t)main_nfc << 16) | ((uint32_t)error << 8) | ((uint32_t)passive);
        if (value && _interruptObserver) _interruptObserver(value, _interruptObserverContext);''')]
}
for name, edits in changes.items():
    p=root/name; raw=p.read_bytes(); newline='\r\n' if b'\r\n' in raw else '\n'
    text=raw.decode().replace('\r\n','\n')
    if '_interruptObserver' in text:
        print(name, 'already patched'); continue
    for old,new in edits:
        if text.count(old)!=1: raise RuntimeError('Unexpected library version: '+name)
        text=text.replace(old,new)
    backup=p.with_suffix(p.suffix+'.ntag215.bak')
    if backup.exists(): raise RuntimeError('Backup already exists: '+str(backup))
    shutil.copy2(p,backup)
    p.write_bytes(text.replace('\n',newline).encode())
    print(name, 'patched; newline preserved', repr(newline))
