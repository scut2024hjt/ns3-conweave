host_n = 0
switch_n = 0
with open('./mix/output/644568745/644568745_out_pfc.txt', 'r') as file:
    for line in file:
        parts = line.strip().split()
        if 1 == int(parts[2]):
            switch_n+=1
        if int(parts[2]) == 0:
            host_n+=1

print("host: %d" % host_n)
print("switch: %d" %  switch_n)
