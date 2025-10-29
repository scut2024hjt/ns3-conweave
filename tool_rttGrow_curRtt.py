#!/usr/bin/env python3

import argparse

parser = argparse.ArgumentParser()
parser.add_argument('-init', '--init', dest='init', type=int, required=False, action='store', help="traceId")
parser.add_argument('-baseInc', '--baseInc', dest='baseInc', type=int, required=False, action='store', help="traceId")
parser.add_argument('-alpha', '--alpha', dest='alpha', type=float, required=False, action='store', help="traceId")
parser.add_argument('-step', '--step', dest='step', type=int, required=False, action='store', help="traceId")
args = parser.parse_args()

def calculate_rtt_growth(initial_rtt, base_inc, alpha, steps):
    rtt_values = [initial_rtt]
    current_rtt = initial_rtt
    
    for _ in range(steps):
        increment = base_inc * (1 + current_rtt * alpha)
        current_rtt += increment
        rtt_values.append(current_rtt)
    
    return rtt_values

# 参数设置：使用命令行参数或默认值
initial_rtt = args.init if args.init is not None else 5000  # 初始RTT，单位ns
base_inc = args.baseInc if args.baseInc is not None else 500      # 基础增量
alpha = args.alpha if args.alpha is not None else 0.005           # 调整系数
steps = args.step if args.step is not None else 100               # 发包次数

# 计算RTT变化
rtt_history = calculate_rtt_growth(initial_rtt, base_inc, alpha, steps)

# 输出结果
print(f"发包次数 | oneway_rtt (ns)")
print("-" * 30)
for i in range(min(21, len(rtt_history))):  # 只显示前10次变化
    print(f"{i:8d} | {rtt_history[i]:.0f}")

print("\n...")
print(f"\n第100次发包后: {rtt_history[100]:.2e} ns")

