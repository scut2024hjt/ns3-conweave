#!/usr/bin/python3

import argparse

parser = argparse.ArgumentParser()
parser.add_argument('-id', '--id', dest='id', required=True, action='store', help="traceId")
args = parser.parse_args()


host_n = 0
switch_n = 0
leaf_n = 0
spine_n = 0
file = "./mix/output/" + args.id + "/" + args.id + "_out_pfc.txt"
with open(file, 'r') as file:
    for line in file:
        parts = line.strip().split()
        if 1 == int(parts[2]):
            switch_n+=1
            if int(parts[1])>=128 and int(parts[1])<=135:
                leaf_n +=1
            elif(int(parts[1]) > 135):
                spine_n +=1
        if int(parts[2]) == 0:
            host_n+=1


print("host: %d" % host_n)
print("switch: %d" %  switch_n)
print("leaf: %d" % leaf_n)
print("spine_n: %d" % spine_n)
