#!/usr/bin/env python3
"""Regresión del rebase de literal pools de __text (tools/macho_prep.py) con casos vistos en consola.

Uso: python3 -I tools/test_macho_prep.py [Executable]   (por defecto game_app/ZombieInfection)
"""
import os, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'game_app', 'ZombieInfection')

CASES = {
    0x207dc: True,   # corrida 1: ldr r3,=g (CIrrFactory) absoluto -> se rebasea
    0x207d8: True,   # vtable de CIrrFactory
    0x2778: False,   # corrida 2: offset PIC usado 10 instrucciones después (add r4, pc, r4)
    0x1feecc: True,  # absoluto (mov r2, r8) tras bx r12 + literal pool que parece instrucción
    0x141518: False, # offset PIC (add r1, pc, r1)
}

with tempfile.TemporaryDirectory() as d:
    out = os.path.join(d, 'rebase.bin')
    subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'macho_prep.py'), BIN, out],
                   check=True, stdout=subprocess.DEVNULL)
    b = open(out, 'rb').read()
    rebase = set(struct.unpack_from(f'<{struct.unpack_from("<I", b, 4)[0]}I', b, 8))
bad = [f'{a:#x} esperado {"en" if want else "fuera de"} rebase.bin' for a, want in CASES.items() if (a in rebase) != want]
print('\n'.join(bad) or f'macho_prep ok ({len(CASES)} casos)')
sys.exit(1 if bad else 0)
