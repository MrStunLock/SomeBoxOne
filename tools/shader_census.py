#!/usr/bin/env python3
"""Census of DXBC shader blobs written by SOMEBOXONE_DUMP_SHADERS=<dir>.
Usage: shader_census.py <dir-with-.bin-files>
Prints shader kinds, DXBC chunk types, shader models and an opcode histogram (how many shaders use each SM4 instruction)."""
import struct, glob, collections, sys, os
NAMES = {0:'add',1:'and',13:'discard',14:'div',15:'dp2',16:'dp3',17:'dp4',18:'else',21:'endif',24:'eq',25:'exp',26:'frc',27:'ftoi',28:'ftou',29:'ge',
         30:'iadd',31:'if',32:'ieq',33:'ige',34:'ilt',35:'imad',38:'imul',39:'ine',41:'ishl',42:'ishr',43:'itof',45:'ld',47:'log',48:'loop',49:'lt',50:'mad',
         51:'min',52:'max',53:'customdata',54:'mov',55:'movc',56:'mul',57:'ne',58:'nop',59:'not',60:'or',61:'resinfo',62:'ret',64:'round_ne',65:'round_ni',
         66:'round_pi',67:'round_z',68:'rsq',69:'sample',70:'sample_c',71:'sample_c_lz',72:'sample_l',73:'sample_d',74:'sample_b',75:'sqrt',77:'sincos',
         78:'udiv',79:'ult',80:'uge',81:'umul',85:'ushr',86:'utof',87:'xor',88:'dcl_resource',89:'dcl_constant_buffer',90:'dcl_sampler',95:'dcl_input',
         96:'dcl_input_sgv',97:'dcl_input_siv',98:'dcl_input_ps',99:'dcl_input_ps_sgv',100:'dcl_input_ps_siv',101:'dcl_output',102:'dcl_output_sgv',
         103:'dcl_output_siv',104:'dcl_temps',105:'dcl_indexable_temp',106:'dcl_global_flags'}
PROGRAM_TYPES = ('ps', 'vs', 'gs', 'hs', 'ds', 'cs')

def chunks(d):
    n = struct.unpack_from('<I', d, 28)[0]
    for o in struct.unpack_from('<%dI' % n, d, 32):
        yield d[o:o + 4].decode('latin1'), o

def main(folder):
    hist, per, chunk_ct, models, kinds = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    files = sorted(glob.glob(os.path.join(folder, '*.bin')))
    for f in files:
        d = open(f, 'rb').read()
        if d[:4] != b'DXBC':
            print('not DXBC:', f, d[:4]); continue
        kinds[os.path.basename(f).split('_', 2)[-1][:-4]] += 1
        for fc, o in chunks(d):
            chunk_ct[fc] += 1
            if fc in ('SHDR', 'SHEX'):
                size = struct.unpack_from('<I', d, o + 4)[0]
                toks = struct.unpack_from('<%dI' % (size // 4), d, o + 8)
                ver = toks[0]; models[(PROGRAM_TYPES[(ver >> 16) & 0xffff], '%d_%d' % ((ver >> 4) & 0xf, ver & 0xf))] += 1
                i, seen = 2, set()
                while i < len(toks):
                    t = toks[i]; op = t & 0x7ff; ln = (t >> 24) & 0x7f
                    if op == 53: ln = toks[i + 1]
                    if ln == 0: break
                    hist[op] += 1; seen.add(op); i += ln
                for op in seen: per[op] += 1
    print('shaders:', len(files), dict(kinds)); print('chunks:', dict(chunk_ct)); print('models:', dict(models))
    print('opcode           uses / shaders-using-it')
    for op, c in sorted(hist.items()): print('  %-20s %5d / %2d' % (NAMES.get(op, op), c, per[op]))

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'research/minecraft_1_2_0_0/shaders')
