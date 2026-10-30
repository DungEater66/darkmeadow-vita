#!/usr/bin/env python3
"""Extract the JNI tables that libUnrealEngine3.so sets up in JNI_OnLoad.
   natives  : JNINativeMethod {name, sig, fnPtr}  (Java -> native), registered with RegisterNatives
   callbacks: {slot*, name*, sig*}                  (native -> Java), resolved with GetMethodID
   usage: extract_jni_tables.py libUnrealEngine3.so > jni_tables.json
   (addresses were located by disassembling JNI_OnLoad of build 'UDKGame-Android-Shipping' v1.4.6.4)"""
import sys, struct, json
from elftools.elf.elffile import ELFFile
NATIVES_VA, NATIVES_N = 0x17aab48, 40
CALLBACKS_VA, CALLBACKS_N = 0x16E8D08, 66
CLASS_VA = 0x165e800
data = open(sys.argv[1], 'rb').read()
elf = ELFFile(open(sys.argv[1], 'rb'))
def v2o(va):
    for s in elf.iter_segments():
        if s['p_type'] == 'PT_LOAD' and s['p_vaddr'] <= va < s['p_vaddr'] + s['p_filesz']:
            return va - s['p_vaddr'] + s['p_offset']
def cstr(va):
    o = v2o(va); return data[o:data.find(b'\0', o)].decode('latin1')
# JNI_OnLoad also looks up these five Java methods separately from the 66-entry table above (the hardware log
# showed GetMethodID calls for them). Their names are strings in the library; the signatures come from the Java
# side (BaseUE3JavaApp in the APK) and from that log. Sound effects are played by Android's SoundPool in Java.
EXTRA_CALLBACKS = [
    {'name': 'JavaCallback_LoadSoundFile', 'sig': '(Ljava/lang/String;)I'},
    {'name': 'JavaCallback_UnloadSoundID', 'sig': '(I)V'},
    {'name': 'JavaCallback_PlaySound',     'sig': '(IZ)I'},
    {'name': 'JavaCallback_StopSound',     'sig': '(I)V'},
    {'name': 'JavaCallback_SetVolume',     'sig': '(IF)V'},
]
out = {'class': cstr(CLASS_VA), 'natives': [], 'callbacks': [], 'extra_callbacks': EXTRA_CALLBACKS}
for i in range(NATIVES_N):
    nm, sg, fp = struct.unpack('<III', data[v2o(NATIVES_VA+12*i):][:12])
    out['natives'].append({'name': cstr(nm), 'sig': cstr(sg), 'fn_offset': fp})
for i in range(CALLBACKS_N):
    slot, nm, sg = struct.unpack('<III', data[v2o(CALLBACKS_VA+12*i):][:12])
    out['callbacks'].append({'name': cstr(nm), 'sig': cstr(sg)})
json.dump(out, sys.stdout, indent=1)
