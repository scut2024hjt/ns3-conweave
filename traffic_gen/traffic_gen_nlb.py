# -*- coding: UTF-8 -*-

import sys
import random
import math
import heapq
from optparse import OptionParser
from custom_rand import CustomRand

class Flow:
    def __init__(self, src, dst, size, t):
        self.src, self.dst, self.size, self.t = src, dst, size, t

    def __str__(self):
        return "%d %d 3 %d %.9f" % (self.src, self.dst, self.size, self.t)

def translate_bandwidth(b):
    if b is None:
        return None
    if type(b) != str:
        return None
    if b[-1] == 'G':
        return float(b[:-1]) * 1e9
    if b[-1] == 'M':
        return float(b[:-1]) * 1e6
    if b[-1] == 'K':
        return float(b[:-1]) * 1e3
    return float(b)

def poisson(lam):
    return -math.log(1 - random.random()) * lam

if __name__ == "__main__":
    port = 80
    parser = OptionParser()
    parser.add_option("-c", "--cdf", dest="cdf_file", help="the file of the traffic size cdf", default="Solar2022.txt")
    parser.add_option("-n", "--nhost", dest="nhost", help="number of hosts")
    parser.add_option("--low_load", dest="low_load", help="the percentage of the low traffic load to the network capacity, by default 0.1", default="0.1")
    parser.add_option("--high_load", dest="high_load", help="the percentage of the high traffic load to the network capacity, by default calculated based on 400Gbps", default="4")
    parser.add_option("--high_time", dest="high_time", help="the duration of high traffic (s), by default 2", default="0.1")
    parser.add_option("--low_time", dest="low_time", help="the duration of low traffic (s), by default 8", default="8")
    parser.add_option("-b", "--bandwidth", dest="bandwidth", help="the bandwidth of host link (G/M/K), by default 10G", default="10G")
    parser.add_option("-t", "--time", dest="time", help="the total run time (s), by default 10", default="10")
    parser.add_option("-o", "--output", dest="output", help="the output file", default="tmp_traffic.txt")
    options, args = parser.parse_args()

    base_t = 2000000000  

    if not options.nhost:
        print("please use -n to enter number of hosts")
        sys.exit(0)
    nhost = int(options.nhost)
    low_load = float(options.low_load)
    link_bandwidth = translate_bandwidth(options.bandwidth)
    if options.high_load is None:
        high_load = 400e9 / link_bandwidth
    else:
        high_load = float(options.high_load)
    high_time = float(options.high_time) * 1e9  
    low_time = float(options.low_time) * 1e9  
    time = float(options.time) * 1e9  
    output = options.output
    if link_bandwidth is None:
        print("bandwidth format incorrect")
        sys.exit(0)

    fileName = options.cdf_file
    file = open(fileName, "r")
    lines = file.readlines()
    # read the cdf, save in cdf as [[x_i, cdf_i] ...]
    cdf = []
    for line in lines:
        x, y = map(float, line.strip().split(' '))
        cdf.append([x, y])

    # create a custom random generator, which takes a cdf, and generate number according to the cdf
    customRand = CustomRand()
    if not customRand.setCdf(cdf):
        print("Error: Not valid cdf")
        sys.exit(0)

    flow_lines = []

    # generate flows
    avg = customRand.getAvg()
    n_flow = 0
    current_time = base_t
    end_time = base_t + time
    is_high_load = False  # 初始为低负载

    while current_time < end_time:
        if is_high_load:
            load = high_load
            period_duration = high_time
        else:
            load = low_load
            period_duration = low_time

        avg_inter_arrival = 1 / (link_bandwidth * load / 8. / avg) * 1000000000
        host_list = [(current_time + int(poisson(avg_inter_arrival)), i) for i in range(nhost)]
        heapq.heapify(host_list)
        period_end = current_time + period_duration

        while len(host_list) > 0 and host_list[0][0] < period_end:
            t, src = host_list[0]
            inter_t = int(poisson(avg_inter_arrival))
            new_tuple = (src, t + inter_t)
            dst = random.randint(0, nhost - 1)
            while dst == src:
                dst = random.randint(0, nhost - 1)
            if t + inter_t > end_time:
                heapq.heappop(host_list)
            else:
                size = int(customRand.rand())
                if size <= 0:
                    size = 1
                n_flow += 1
                flow_lines.append("%d %d 3 %d %.9f\n" % (src, dst, size, t * 1e-9))
                heapq.heapreplace(host_list, (t + inter_t, src))

        is_high_load = not is_high_load
        current_time = period_end

    
    with open(output, "w") as ofile:
        ofile.write("%d\n" % n_flow)
        for line in flow_lines:
            ofile.write(line)