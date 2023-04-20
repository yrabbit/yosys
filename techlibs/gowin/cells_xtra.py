#!/usr/bin/env python3

# Base on Nexus cells_xtra.py

from argparse import ArgumentParser
import os.path
from enum import Enum, auto
import sys
import re

class State(Enum):
    OUTSIDE = auto()
    IN_MODULE = auto()

def xtract_cells_decl(dir, fout):
    fname = os.path.join(dir, 'prim_sim.v')
    with open(fname) as f:
        state = State.OUTSIDE
        for l in f:
            l, _, comment = l.partition('//')
            if l.startswith("module "):
                cell_name = l[7:l.find('(')].strip()
                print(cell_name)

if __name__ == '__main__':
    parser = ArgumentParser(description='Extract Gowin blackbox cell definitions.')
    parser.add_argument('gowin_dir', nargs='?', default='/opt/gowin/')
    args = parser.parse_args()

    dirs = [
        os.path.join(args.gowin_dir, 'IDE/simlib/gw1n/'),
    ]

    with open('cells_xtra.v', 'w') as fout:
        fout.write('// Created by cells_xtra.py\n')
        fout.write('\n')
        for dir in dirs:
            if not os.path.isdir(dir):
                print(f'{dir} is not a directory')
            xtract_cells_decl(dir, fout)
