#!/usr/bin/python3

import numpy as np
import subprocess
import argparse

def calculate_ideal_afct(input_file, time_limit_start, time_limit_end):
    cmd = f"cat {input_file} | awk '{{if ($6 > {time_limit_start} && $6 + $7 < {time_limit_end}) {{print $8/1000}}}}'"

    output = subprocess.check_output(cmd, shell=True)
    
    # 将输出转换为 FCT 列表（单位：微秒）
    fct_list = [float(x) for x in output.decode("utf-8").split()]

    # 计算所有流量的平均 FCT
    if len(fct_list) > 0:
        total_avg_fct = np.mean(fct_list)
        return total_avg_fct
    else:
        return None  # 如果没有流量数据，返回 None

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='计算所有流量的平均 FCT')
    parser.add_argument('-id', '--id', dest='id', required=True, action='store', help="traceId")
    parser.add_argument('-st', dest='simul_time', action='store',
                    default='0.1', help="traffic time to simulate (up to 3 seconds) (default: 0.1)")

    args = parser.parse_args()

    flowgen_start_time = 2.0  # default: 2.0
    flowgen_stop_time = flowgen_start_time + \
        float(args.simul_time)  # default: 2.0
    fct_analysis_time_limit_begin = int(
        flowgen_start_time * 1e9) + int(0.005 * 1e9)  # warmup
    fct_analysistime_limit_end = int(
        flowgen_stop_time * 1e9) + int(0.05 * 1e9)  # extra term

    input_file = "./mix/output/" + args.id + "/" + args.id + "_out_fct.txt"
    ideal_afct = calculate_ideal_afct(input_file, fct_analysis_time_limit_begin, fct_analysistime_limit_end)
    print(ideal_afct)