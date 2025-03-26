total = 0
with open('./mix/output/398534747/398534747_out_cnp.txt', 'r') as file:
    for line in file:
        parts = line.strip().split()
        if len(parts) >= 5:
            total += int(parts[4])

print(total)
