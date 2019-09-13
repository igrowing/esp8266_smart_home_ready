#!/usr/bin/env python
import re
import sys
from pprint import pprint

print 'Converting CSV input file with raw IR code into nested dictionary of ready IRsend commands for Tasmota\n'
if len(sys.argv) < 2:
    print 'Usage: provide file name as argument\n'
    sys.exit(1)

f = open(sys.argv[1], 'r')
lines = f.readlines()

db = {}
for l in lines[1:]:  # skip header
    mode, speed, temp = l.split(',')[:3]
    db.setdefault(mode, {})
    db[mode].setdefault(speed, {})
    raw = eval(re.findall('RawData\"\"\:(.*)\,\"\"RawData', l)[0])
    out = ''
    for i in raw[3::2]:
        if i > 1000:
            one = i
            out += '1'
        else:
            zero = i
            out += '0'
    # print mode, speed, temp, raw[0], raw[1], raw[2], zero, one, out
    db[mode][speed][temp] = 'irsend raw,0,%s,%s,%s,%s,%s,%s' % (raw[0], raw[1], raw[2], zero, one, out)

pprint(db)
