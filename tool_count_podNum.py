#!/usr/bin/python3


fileName = "./config/L_40.00_CDF_webserver_N_128_T_10ms_B_100_flow.txt"
pod_num = 0
with open(fileName, 'r') as file:
	num_lines = int(file.readline().strip())
	for _ in range(num_lines):
		line = file.readline().strip()
		if line:
			 columns = list(map(float, line.split()))
			 if (columns[0]//16 == columns[1]//16):
			 	pod_num += 1

print(pod_num);
