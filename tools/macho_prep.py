#!/usr/bin/env python3
"""Prepara el ejecutable iOS (Mach-O 32 bits, MH_EXECUTE no PIE, armv6/armv7) para el loader de la PS Vita.

Soporta binarios thin (0xFEEDFACE) y fat/universal (0xCAFEBABE) seleccionando el slice ARM óptimo.
Detecta cifrado FairPlay (LC_ENCRYPTION_INFO).
Calcula los punteros absolutos que requieren reubicación en memoria física (rebase.bin)
y extrae el inventario completo de imports dinámicos.

Uso: python3 -I macho_prep.py <Executable> <rebase.bin> [--report report.txt] [--arch armv7|armv6]
"""
import bisect
import os
import struct
import sys
from collections import Counter

LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB
LC_ENCRYPTION_INFO = 0x21
S_ZEROFILL = 0x1
S_NON_LAZY = 0x6
S_LAZY = 0x7
INDIRECT_LOCAL = 0x80000000
INDIRECT_ABS = 0x40000000
N_STAB = 0xE0
N_TYPE = 0x0E
N_SECT = 0x0E
N_EXT = 0x01
N_ARM_THUMB_DEF = 0x0008

CPU_TYPE_ARM = 12
CPU_SUBTYPE_ARM_V6 = 6
CPU_SUBTYPE_ARM_V7 = 9
CPU_SUBTYPE_ARM_V7S = 11


def parse(path, preferred_arch=None):
    with open(path, 'rb') as f:
        raw_data = f.read()

    slice_offset = 0
    slice_size = len(raw_data)
    detected_arch = "unknown"

    # Chequear si es FAT Mach-O (big-endian 0xCAFEBABE)
    if len(raw_data) >= 8 and raw_data[:4] in (b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
        magic, nfat = struct.unpack_from('>2I', raw_data, 0)
        archs = []
        for i in range(nfat):
            cputype, cpusubtype, offset, size, align = struct.unpack_from('>5I', raw_data, 8 + i * 20)
            arch_name = "arm"
            if cputype == CPU_TYPE_ARM:
                if cpusubtype == CPU_SUBTYPE_ARM_V6:
                    arch_name = "armv6"
                elif cpusubtype == CPU_SUBTYPE_ARM_V7:
                    arch_name = "armv7"
                elif cpusubtype == CPU_SUBTYPE_ARM_V7S:
                    arch_name = "armv7s"
            archs.append((arch_name, cputype, cpusubtype, offset, size))

        # Seleccionar slice
        chosen = None
        if preferred_arch:
            for a in archs:
                if a[0] == preferred_arch.lower():
                    chosen = a
                    break
        if not chosen:
            # Preferir armv7 sobre armv6
            for a in archs:
                if a[0] in ("armv7", "armv7s"):
                    chosen = a
                    break
        if not chosen:
            for a in archs:
                if a[0] == "armv6":
                    chosen = a
                    break
        if not chosen and archs:
            chosen = archs[0]

        if not chosen:
            raise RuntimeError("No se encontró ningún slice ARM en el binario FAT")

        slice_offset = chosen[3]
        slice_size = chosen[4]
        detected_arch = chosen[0]
        d = raw_data[slice_offset:slice_offset + slice_size]
    else:
        d = raw_data

    magic, cpu, sub, ftype, ncmds, _sz, flags = struct.unpack_from('<7I', d, 0)
    if magic != 0xFEEDFACE:
        raise RuntimeError(f"Se espera Mach-O de 32 bits (magic 0xFEEDFACE), obtenido 0x{magic:08X}")
    if ftype != 2:
        print(f"[!] Advertencia: Mach-O ftype={ftype} (se esperaba 2 = MH_EXECUTE)")

    if cpu == CPU_TYPE_ARM:
        if sub == CPU_SUBTYPE_ARM_V6:
            detected_arch = "armv6"
        elif sub == CPU_SUBTYPE_ARM_V7:
            detected_arch = "armv7"
        elif sub == CPU_SUBTYPE_ARM_V7S:
            detected_arch = "armv7s"
        else:
            detected_arch = f"arm(sub={sub})"

    segs, sects, symtab, dysym = [], [], None, None
    cryptid = 0
    off = 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', d, off)
        if cmd == LC_SEGMENT:
            name = d[off + 8:off + 24].rstrip(b'\0').decode('latin1', errors='ignore')
            vmaddr, vmsize, fileoff, filesize, _mp, _ip, nsects, _f = struct.unpack_from('<8I', d, off + 24)
            segs.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff, filesize=filesize))
            so = off + 56
            for _ in range(nsects):
                sn = d[so:so + 16].rstrip(b'\0').decode('latin1', errors='ignore')
                gn = d[so + 16:so + 32].rstrip(b'\0').decode('latin1', errors='ignore')
                addr, ssize, soff, _al, _ro, _nr, sflags, r1, r2 = struct.unpack_from('<9I', d, so + 32)
                sects.append(dict(seg=gn, name=sn, addr=addr, size=ssize, offset=soff,
                                  type=sflags & 0xFF, r1=r1))
                so += 68
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from('<4I', d, off + 8)
        elif cmd == LC_DYSYMTAB:
            dysym = struct.unpack_from('<18I', d, off + 8)
        elif cmd == LC_ENCRYPTION_INFO:
            cryptoff, cryptsize, cryptid = struct.unpack_from('<3I', d, off + 8)
        off += size

    return d, segs, sects, symtab, dysym, detected_arch, cryptid


def main():
    if len(sys.argv) < 3:
        print("Uso: python3 -I macho_prep.py <Executable> <rebase.bin> [--report report.txt] [--arch armv7|armv6]")
        sys.exit(1)

    src, out = sys.argv[1], sys.argv[2]
    report = sys.argv[sys.argv.index('--report') + 1] if '--report' in sys.argv else None
    preferred_arch = sys.argv[sys.argv.index('--arch') + 1] if '--arch' in sys.argv else None

    d, segs, sects, symtab_cmd, dysym, detected_arch, cryptid = parse(src, preferred_arch)

    print(f"[*] Binario: {src}")
    print(f"[*] Arquitectura Mach-O: {detected_arch}")
    if cryptid != 0:
        print(f"[!] ALERTA CRITICA: El binario esta CIFRADO por FairPlay DRM (cryptid = {cryptid})!")
        print(f"[!] Debe ser descifrado en un dispositivo iOS (con Clutch, frida-ios-dump o bfdecrypt) antes de correr.")
    else:
        print(f"[+] Binario descifrado (cryptid = 0), listo para ejecución.")

    if not symtab_cmd:
        raise RuntimeError("No se encontró tabla de símbolos (LC_SYMTAB)")
    symoff, nsyms, stroff, _strsize = symtab_cmd

    lo = min(s['vmaddr'] for s in segs if s['vmsize'] and s['name'] != '__PAGEZERO')
    hi = max(s['vmaddr'] + s['vmsize'] for s in segs if s['name'] != '__LINKEDIT')

    def read32(va):
        for s in segs:
            if s['vmaddr'] <= va < s['vmaddr'] + s['filesize']:
                return struct.unpack_from('<I', d, s['fileoff'] + va - s['vmaddr'])[0]
        return 0

    def sect_of(va):
        for s in sects:
            if s['addr'] <= va < s['addr'] + s['size']:
                return s
        return None

    # Símbolos definidos
    syms, names = [], []
    for i in range(nsyms):
        strx, ntype, nsect, ndesc, value = struct.unpack_from('<IBBhI', d, symoff + i * 12)
        idx_end = d.find(b'\0', stroff + strx)
        name = d[stroff + strx:idx_end].decode('latin1', errors='ignore') if idx_end != -1 else ""
        names.append(name)
        if ntype & N_STAB or (ntype & N_TYPE) != N_SECT:
            continue
        syms.append((value, bool(ndesc & N_ARM_THUMB_DEF), name))
    syms.sort()
    sym_addrs = [s[0] for s in syms]

    text_sect = next((s for s in sects if s['name'] == '__text'), None)
    func_entry = {}
    if text_sect:
        for a, thumb, n in syms:
            if text_sect['addr'] <= a < text_sect['addr'] + text_sect['size']:
                func_entry[a] = n
                func_entry[a | 1] = n

    stubs = next((s for s in sects if s['name'] in ('__symbol_stub4', '__symbol_stub', '__symbol_stub1')), None)
    if stubs:
        step = 12 if stubs['name'] == '__symbol_stub4' else 16
        for a in range(stubs['addr'], stubs['addr'] + stubs['size'], step):
            func_entry[a] = 'stub'

    def valid_target(v):
        if not (lo <= v < hi):
            return None
        s = sect_of(v & ~1)
        if s is None:
            return None
        if s['name'] in ('__text', '__symbol_stub4', '__symbol_stub'):
            return 'func' if v in func_entry else None
        if s['name'] in ('__cstring', '__ustring'):
            ok = v == s['addr'] or (read32(v - 1) & 0xFF == 0 if s['name'] == '__cstring' else True)
            return 'cstr' if ok else None
        if v & 3 and s['name'] not in ('__const', '__data'):
            return None
        i = bisect.bisect_right(sym_addrs, v) - 1
        if i >= 0 and sym_addrs[i] == v:
            return 'sym'
        if s['type'] == S_ZEROFILL or s['name'] in ('__common', '__bss'):
            return 'bss-interior' if i >= 0 else None
        return 'interior' if v == s['addr'] or (i >= 0 and v - sym_addrs[i] < 0x10000) else None

    handled, imports = set(), Counter()
    local_ptr_slots = []
    if dysym:
        indirectsymoff, nindirect = dysym[12], dysym[13]
        extreloff, nextrel = dysym[14], dysym[15]
        for s in sects:
            if s['type'] in (S_NON_LAZY, S_LAZY):
                for i in range(s['size'] // 4):
                    idx = struct.unpack_from('<I', d, indirectsymoff + (s['r1'] + i) * 4)[0]
                    slot = s['addr'] + i * 4
                    handled.add(slot)
                    if idx & INDIRECT_LOCAL:
                        local_ptr_slots.append(slot)
                    elif not idx & INDIRECT_ABS:
                        if idx < len(names):
                            imports[names[idx]] += 1
        extrel_slots = []
        for i in range(nextrel):
            addr, info = struct.unpack_from('<iI', d, extreloff + i * 8)
            symnum = info & 0xFFFFFF
            va = addr
            extrel_slots.append((va, names[symnum] if symnum < len(names) else f"sym_{symnum}"))
            handled.add(va)
            if symnum < len(names):
                imports[names[symnum]] += 1
    else:
        extrel_slots = []

    rebase, stats, rejected, rejected_list = [], Counter(), Counter(), []
    scan = [s for s in sects if s['type'] != S_ZEROFILL and s['name'] not in ('__text', '__symbol_stub4', '__symbol_stub', '__cstring', '__ustring')
            and s['seg'] in ('__DATA', '__TEXT')]

    owner_ok, owner_bad, owner_hits = Counter(), Counter(), {}
    for s in scan:
        for va in range(s['addr'] + (-s['addr'] & 3), s['addr'] + s['size'] - 3, 4):
            if va in handled:
                continue
            v = read32(va)
            k = valid_target(v)
            owner = None
            if s['name'] in ('__data', '__const'):
                i = bisect.bisect_right(sym_addrs, va) - 1
                owner = syms[i][2] if i >= 0 and sym_addrs[i] >= s['addr'] else f'{s["seg"]}.{s["name"]}'
            if k:
                if owner:
                    owner_ok[owner] += 1
                    owner_hits.setdefault(owner, []).append((va, s['name'], k))
                else:
                    rebase.append(va)
                    stats[(s['name'], k)] += 1
            elif lo <= v < hi:
                rejected[s['name']] += 1
                rejected_list.append((va, v))
                if owner:
                    owner_bad[owner] += 1

    dropped = []
    for owner, hits in owner_hits.items():
        if owner_bad[owner] > owner_ok[owner]:
            dropped.append((owner, owner_ok[owner], owner_bad[owner]))
            # Conservar destinos a símbolos reales
            for va, sn, k in hits:
                if k in ('sym', 'func', 'cstr'):
                    rebase.append(va)
                    stats[(sn, k)] += 1
            continue
        for va, sn, k in hits:
            rebase.append(va)
            stats[(sn, k)] += 1

    rebase += local_ptr_slots
    stats[('__nl_symbol_ptr', 'local')] = len(local_ptr_slots)

    # Literal pools de __text. Los ejecutables non-PIE no traen relocaciones locales y el código
    # compilado con -mdynamic-no-pic (Irrlicht) guarda direcciones absolutas en sus literal pools.
    # Un literal cargado con `ldr Rt, [pc, #n]` es un offset PIC si Rt se usa enseguida con pc
    # (ARM: add Rd, pc, Rt / ldr|str Rd, [pc, Rt]; Thumb: add Rt, pc); si no, es absoluto y se
    # rebasea cuando apunta a un destino válido (mismo criterio que los datos).
    if text_sect:
        t0, t1 = text_sect['addr'], text_sect['addr'] + text_sect['size']
        thumb_funcs = sorted((a, th) for a, th, _ in syms if t0 <= a < t1)
        starts = [a for a, _ in thumb_funcs]

        def is_thumb(va):
            i = bisect.bisect_right(starts, va) - 1
            return i >= 0 and thumb_funcs[i][1]

        def read16(va):
            return read32(va & ~3) >> (16 * ((va >> 1) & 1)) & 0xFFFF

        def func_end(va):
            i = bisect.bisect_right(starts, va)
            return starts[i] if i < len(starts) else t1

        # El compilador puede separar la carga del literal y su uso con pc por varias instrucciones
        # (se vio a 10): se sigue Rt hasta que se redefine, hasta un return o el fin de la función.
        def arm_pic_use(va, rt):
            end = min(func_end(va), va + 4 * 64)
            for a in range(va + 4, end, 4):
                if a in arm_pool:
                    continue  # dato del literal pool, no instrucción
                n = read32(a)
                if (n & 0x0FEF0FFF) in (0x008F0000 | rt, 0x00800000 | rt << 16 | 15) or \
                        (n & 0x0E0F0FFF) == (0x060F0000 | rt) or \
                        ((n & 0x0E4F0F90) == 0x000F0090 and n & 15 == rt):
                    return True
                if n >> 28 != 0xE:
                    continue  # condicional: no redefine Rt en todos los caminos
                op = n >> 25 & 7
                if n & 0x0FFFFFF0 == 0x012FFF10 or (op == 4 and n & 1 << 20 and n & 1 << 15):
                    return False  # bx Rm / ldm {..., pc}: fin del flujo
                if op == 4 and n & 1 << 20 and n & 1 << rt:
                    return False  # ldm/pop que recarga Rt
                writes = (op in (0, 1) and not (op == 0 and n & 0x90 == 0x90) and not 8 <= (n >> 21 & 15) <= 11) \
                    or (op in (2, 3) and n & 1 << 20)
                if writes and n >> 12 & 15 == rt:
                    return False
            return False

        def thumb_pic_use(va, rt):
            end = min(func_end(va), va + 2 * 64)
            for a in range(va + 2, end, 2):
                if a & ~3 in thumb_pool:
                    continue
                h = read16(a)
                if h == 0x4478 | rt:
                    return True
                if h & 0xF800 == 0x4800 and h >> 8 & 7 == rt:
                    return False  # otro literal en Rt
                if h & 0xFF00 == 0xBD00 or h == 0x4770:
                    return False  # pop {..., pc} / bx lr
            return False

        arm_pool = set()
        for va in range(t0 & ~3, t1 - 3, 4):
            insn = read32(va)
            if not is_thumb(va) and insn & 0x0F7F0000 == 0x051F0000 and insn >> 28 != 0xF:
                imm = insn & 0xFFF
                arm_pool.add(va + 8 + (imm if insn & 1 << 23 else -imm))

        thumb_pool = set()
        for va in range(t0 & ~1, t1 - 1, 2):
            h = read16(va)
            if is_thumb(va) and h & 0xF800 == 0x4800:
                thumb_pool.add(((va + 4) & ~3) + (h & 0xFF) * 4)

        lit_abs, lit_pic = set(), set()
        for va in range(t0 & ~3, t1 - 3, 4):  # ARM
            if is_thumb(va):
                continue
            insn = read32(va)
            if insn & 0x0F7F0000 != 0x051F0000 or insn >> 28 == 0xF:
                continue
            rt, imm = insn >> 12 & 15, insn & 0xFFF
            lit = va + 8 + (imm if insn & 1 << 23 else -imm)
            if not (t0 <= lit < t1) or lit & 3:
                continue
            (lit_pic if arm_pic_use(va, rt) else lit_abs).add(lit)
        for va in range(t0 & ~1, t1 - 1, 2):  # Thumb-1
            if not is_thumb(va):
                continue
            h = read16(va)
            if h & 0xF800 != 0x4800:
                continue
            rt, lit = h >> 8 & 7, ((va + 4) & ~3) + (h & 0xFF) * 4
            if not (t0 <= lit < t1):
                continue
            (lit_pic if thumb_pic_use(va, rt) else lit_abs).add(lit)
        # Tablas de switch non-PIC: `ldrCC pc, [pc, Rm, lsl #2]` + `b default` + direcciones
        # absolutas de los case (dentro de la misma función).
        for va in range(t0 & ~3, t1 - 3, 4):
            if is_thumb(va) or read32(va) & 0x0FFFFFF0 != 0x079FF100 or read32(va) >> 28 == 0xF:
                continue
            i = bisect.bisect_right(starts, va) - 1
            f0 = starts[i] if i >= 0 else t0
            f1 = starts[i + 1] if i + 1 < len(starts) else t1
            e = va + 8
            while f0 <= read32(e) < f1 and not read32(e) & 3:
                rebase.append(e)
                stats[('__text', 'jumptable')] += 1
                e += 4
        for lit in sorted(lit_abs - lit_pic - set(rebase)):
            v = read32(lit)
            k = valid_target(v)
            if k:
                rebase.append(lit)
                stats[('__text', 'lit-' + k)] += 1
            elif lo <= v < hi:
                rejected['__text-lit'] += 1
                rejected_list.append((lit, v))

    if stubs and stubs['name'] == '__symbol_stub4':
        stub_lits = list(range(stubs['addr'] + 8, stubs['addr'] + stubs['size'], 12))
        valid_stubs = [a for a in stub_lits if read32(a - 8) == 0xE59FC000 and lo <= read32(a) < hi]
        rebase += valid_stubs
        stats[('__symbol_stub4', 'la_ptr')] = len(valid_stubs)

    rebase.sort()
    # Eliminar duplicados si los hubiera
    rebase = sorted(list(set(rebase)))

    os.makedirs(os.path.dirname(os.path.abspath(out)) or '.', exist_ok=True)
    with open(out, 'wb') as f:
        f.write(struct.pack('<4sI', b'RBS1', len(rebase)))
        f.write(struct.pack(f'<{len(rebase)}I', *rebase))

    lines = [
        f'Binario: {src} ({detected_arch})',
        f'FairPlay cryptid: {cryptid} ({"CIFRADO" if cryptid else "descifrado"})',
        f'Rango imagen: {lo:#x}-{hi:#x}',
        f'Rebase: {len(rebase)} palabras -> {out}',
    ]
    lines += [f'  {sn:20} {k:14} {n}' for (sn, k), n in sorted(stats.items())]
    lines += [f'Rechazadas (en rango, sin símbolo válido): {dict(rejected)}']
    lines += [f'Objetos descartados (tablas mixtas): {len(dropped)}']
    for o, a, b in sorted(dropped, key=lambda x: -x[1])[:30]:
        lines.append(f'  {o} ok={a} malas={b}')
    lines += [f'Relocs externas: {len(extrel_slots)}; imports únicos: {len(imports)}']
    lines += [f'  {n}' for n in sorted(imports)]

    print('\n'.join(lines[:len(stats) + 5]))
    if report:
        os.makedirs(os.path.dirname(os.path.abspath(report)) or '.', exist_ok=True)
        with open(report, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
        print(f"[+] Reporte completo guardado en {report}")


if __name__ == '__main__':
    main()
