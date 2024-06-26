#!/usr/bin/env python3

import numpy as np
import sys
import os
import petar
import getopt

if __name__ == '__main__':

    data_type = 'single,binary'
    replace_flag = False
    snapshot_format = 'ascii'
    output_format = 'npy'

    def usage():
        print("A tool to convert the file format of post-processed snapshots generated from petar.data.process.")
        print("  PS: another tool, 'petar.format.transfer,' is used for snapshots generated from petar.")
        print("Usage: petar.format.transfer.post [option] [snapshot path list filename]")
        print("  snapshot path list file: A list of snapshot data paths, each line for one snapshot.")
        print("                           This file can be generated by petar.data.gether.")
        print("  Supported file formats:")
        print("    ascii:  Text format generated by numpy.savetxt, all data types are floating-point.")
        print("    binary: Binary format generated by numpy.tofile, data types of columns are preserved.")
        print("             Please note that this is a file format for saving data and should not be confused with the physical binary of stars.")
        print("    npy:    Binary format generated by numpy.save, data types of columns are recorded in the header.")
        print("            The npy format can be directly read by numpy.load without using the petar tool.")
        print("  Please ensure that the stellar evolution method (-i), external mode (-t), and binary information (-B) used in petar.data.process are the same here.")
        print("Options (default arguments shown in parentheses at the end):")
        print("  -h(--help)                Display help information.")
        print("  -i(--interrupt-mode)  [S] The interruption mode used in petar; choices: no, base, bse, mobse (default: no).")
        print("  -t(--external-mode)   [S] External mode used in petar; choices: galpy, no (default: no).")
        print("  -B(--full-binary)         If petar.data.process uses the -B option to obtain full (physical) binary orbital parameters, ")
        print("                            this option is needed to correctly read (physical) binary snapshots.")
        print("  -s(--snapshot-format) [S] Input snapshot file format: ascii, binary, or npy (default: ascii).")
        print("  -o(--output-format)   [S] Output snapshot file format: ascii, binary, or npy (default: npy).")
        print("  -d(--data-type)       [S] Data type of snapshots. Multiple types can be combined by ',' without empty spaces (default: single,binary).")
        print("                            Supported type list:")
        print("                               single: Single snapshots from petar.data.process.")
        print("                               binary: Physical binary snapshots from petar.data.process.")
        print("                               triple: Triple snapshots from petar.data.process.")
        print("                               quadruple: Quadruple snapshots from petar.data.process.")
        print("                            For multiple types, combine the names with ',' such as 'single,binary'.")
        print("  -r(--replace)             Replace snapshots instead of generating new snapshots for file formats of ascii and binary.")
        print("                            If not used, new snapshots are generated with an additional suffix of filename '.a' (ascii), '.b' (binary).")
        print("                            For npy format, new snapshots are always generated with the suffix '.npy'.")
    
    try:
        shortargs = 'i:t:s:o:d:rBh'
        longargs = ['external-mode=','interrupt-mode=','snapshot-format=','output-format=','data-type=','replace','full-binary','help']
        opts,remainder= getopt.getopt( sys.argv[1:], shortargs, longargs)

        snapshot_kwargs=dict()
        for opt,arg in opts:
            if opt in ('-h','--help'):
                usage()
                sys.exit(1)
            elif opt in ('-i','--interrupt-mode'):
                snapshot_kwargs['interrupt_mode'] = arg
            elif opt in ('-t','--external-mode'):
                snapshot_kwargs['external_mode'] = arg
            elif opt in ('-s','--snapshot-format'):
                snapshot_format = arg
            elif opt in ('-o','--output-format'):
                output_format = arg
            elif opt in ('-d','--data-type'):
                data_type = arg
            elif opt in ('-r', '--replace'):
                replace_flag = True
            elif opt in ('-B','--full-binary'):
                snapshot_kwargs['simple_mode'] = False
            else:
                assert False, "unhandeld option"

    except getopt.GetoptError:
        print('getopt error!')
        usage()
        sys.exit(1)

    filename = remainder[0]
    fl = open(filename,'r')
    file_list = fl.read()
    path_list = file_list.splitlines()

    data_type_list = data_type.split(',')
    for file_path in path_list:
        print('convert %s' % file_path)
        for dtype in data_type_list:
            data = None
            if (dtype == 'single'):
                data = petar.Particle(**snapshot_kwargs)
            elif (dtype == 'binary'):
                data = petar.Binary(member_particle_type=petar.Particle, **snapshot_kwargs)
            elif (dtype == 'triple'):
                data = petar.Binary(member_particle_type_one=petar.Particle, member_particle_type_two=[petar.Particle, petar.Particle], **snapshot_kwargs)
            elif (dtype == 'quadruple'):
                data = petar.Binary(member_particle_type=[petar.Particle, petar.Particle], **snapshot_kwargs)

            file_path_full = '%s.%s' % (file_path, dtype)
            if (snapshot_format == 'ascii'):
                data.loadtxt(file_path_full)
            elif (snapshot_format == 'binary'):
                data.fromfile(file_path_full)
            elif (snapshot_format == 'npy'):
                data.load(file_path_full+'.npy')
            else:
                raise ValueError('Error: snapshot format %s is not supported' % snapshot_format)
            out_path = file_path_full
            if (output_format == 'ascii'):
                if (not replace_flag): 
                    out_path = file_path_full+'.a'
                data.savetxt(out_path)
            elif (output_format == 'binary'):
                if (not replace_flag): 
                    out_path = file_path_full+'.b'
                data.tofile(out_path)
            elif (output_format == 'npy'):
                if (not replace_flag): 
                    out_path = file_path_full+'.npy'
                data.save(out_path)
            else:
                raise ValueError('Error: output format %s is not supported' % snapshot_format)

