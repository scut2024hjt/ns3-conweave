#!/usr/bin/python3

import argparse
import os
import re

parser = argparse.ArgumentParser()
parser.add_argument('-id', '--id', dest='id', required=True, action='store', help="traceId")
args = parser.parse_args()

def extract_getbetter_numbers(filename):
    numbers = 0
    pattern = r"getbetter:\s*(-?\d*\.?\d+)"  # 匹配整数或浮点数
    
    try:
        with open(filename, 'r') as file:
            for line in file:
                # 查找所有匹配的数字
                matches = re.findall(pattern, line)
                for num_str in matches:
                    numbers = int(num_str)
                    # # 转换为整数或浮点数
                    # try:
                    #     if '.' in num_str:
                    #         numbers.append(float(num_str))
                    #     else:
                    #         numbers.append(int(num_str))
                    # except ValueError:
                    #     continue  # 忽略转换失败的情况
    except FileNotFoundError:
        print(f"错误：文件 '{filename}' 未找到")
        return []
    
    return numbers


os.system("./tool_absolute_fct_fixed.py -id {id}".format(id=args.id))
os.system("./tool_count_cnp.py -id {id}".format(id=args.id))
os.system("./tool_count_ooo.py -id {id}".format(id=args.id))


input_file = "./mix/output/" + args.id + "/" + "config.log"
getbetter_num = extract_getbetter_numbers(input_file)
print(f"重路由：{getbetter_num}")
