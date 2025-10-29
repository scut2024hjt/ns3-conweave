#!/usr/bin/python3

import argparse

parser = argparse.ArgumentParser()
parser.add_argument('-id', '--id', dest='id', required=True, action='store', help="traceId")
args = parser.parse_args()


total = 0
file = "./mix/output/" + args.id + "/" + args.id + "_out_cnp.txt"
with open(file, 'r') as file:
    for line in file:
        parts = line.strip().split()
        if len(parts) >= 5:
            total += int(parts[3])

print(total)
