#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
绘制多条 CDF 曲线到同一张图，并可通过插值与移动平均平滑曲线。
保证每条曲线从图像最左侧开始（对数轴时选最小正值）。
用法示例：
  python3 plot_multiple_cdfs.py FbHdp2015.txt transformer_moe_cdf.txt --out all_cdfs.png --smooth 2000 --ma 5
  python3 plot_multiple_cdfs.py --out all_cdfs.png --logx --smooth 1000
"""
import argparse
import os
import glob
import numpy as np
import matplotlib.pyplot as plt

def read_cdf_file(path):
    xs = []
    ys = []
    with open(path, 'r') as f:
        for ln in f:
            ln = ln.strip()
            if not ln or ln.startswith('#') or ln.upper().startswith('REM'):
                continue
            parts = ln.split()
            if len(parts) < 2:
                continue
            try:
                x = float(parts[0])
                y = float(parts[1])
            except:
                continue
            xs.append(x)
            ys.append(y)
    if not xs:
        return None, None
    xs = np.array(xs)
    ys = np.array(ys)
    # 如果第二列看起来是百分比（max>1），则除以100
    if ys.max() > 1.0:
        ys = ys / 100.0
    # 确保按 x 排序并去重（插值要求严格单调）
    order = np.argsort(xs)
    xs = xs[order]
    ys = ys[order]
    # 去除重复 x（保留最后一个对应的 y）
    if np.any(np.diff(xs) == 0):
        uniq_x, idx = np.unique(xs, return_index=True)
        # np.unique 返回第一个索引，使用后面的值更保守：取最后出现位置
        last_indices = [np.where(xs == ux)[0][-1] for ux in uniq_x]
        xs = xs[last_indices]
        ys = ys[last_indices]
    return xs, ys

def smooth_cdf(xs, ys, npoints=0, ma_window=1, logx=False):
    """
    xs, ys: 原始点（xs 单调递增，ys 单调非减）
    npoints: 插值到的点数（<=0 表示不插值）
    ma_window: 移动平均窗口（1 表示不平滑）
    logx: 如果 x 轴为对数，插值在 log 空间进行
    返回：xs_out, ys_out（保证 ys 单调且在 [0,1]）
    """
    if npoints is None or npoints <= 0 or len(xs) < 2:
        xs_out = xs
        ys_out = ys
    else:
        if logx:
            # 防止非正值进入 log
            if xs[0] <= 0:
                # 将最小正值替换为一个很小的正数
                eps = 1e-9
                xs_for_log = np.copy(xs)
                xs_for_log[xs_for_log <= 0] = eps
            else:
                xs_for_log = xs
            x_min_log = np.log10(xs_for_log.min())
            x_max_log = np.log10(xs_for_log.max())
            xs_dense = np.logspace(x_min_log, x_max_log, num=npoints, base=10.0)
            # 对应的插值使用原始 xs（若有非正值插值会异常，已处理）
            ys_dense = np.interp(xs_dense, xs_for_log, ys, left=ys[0], right=ys[-1])
            xs_out = xs_dense
            ys_out = ys_dense
        else:
            xs_dense = np.linspace(xs.min(), xs.max(), num=npoints)
            ys_dense = np.interp(xs_dense, xs, ys, left=ys[0], right=ys[-1])
            xs_out = xs_dense
            ys_out = ys_dense

    # 移动平均（简单卷积），保留边界长度
    if ma_window is None:
        ma_window = 1
    ma = int(max(1, ma_window))
    if ma > 1:
        kernel = np.ones(ma) / ma
        ys_smooth = np.convolve(ys_out, kernel, mode='same')
        ys_out = ys_smooth

    # 保证单调非减（CDF 必须单调），并裁剪到 [0,1]
    ys_out = np.maximum.accumulate(ys_out)
    ys_out = np.clip(ys_out, 0.0, 1.0)

    return xs_out, ys_out

def main():
    p = argparse.ArgumentParser(description="Plot multiple CDF files on one figure (with smoothing).")
    p.add_argument('files', nargs='*', help="CDF files (if empty, will use all *.txt in current dir)")
    p.add_argument('--out', '-o', default='multi_cdf.png', help="output image file")
    p.add_argument('--logx', action='store_true', help="use log scale on x axis")
    p.add_argument('--xlabel', default='Value', help='x axis label')
    p.add_argument('--ylabel', default='CDF', help='y axis label')
    p.add_argument('--smooth', '-s', type=int, default=0, help="number of interpolation points (0=no interpolation)")
    p.add_argument('--ma', type=int, default=1, help="moving average window size (1=no MA smoothing)")
    p.add_argument('--linewidth', type=float, default=1.5, help="line width")
    args = p.parse_args()

    if not args.files:
        args.files = sorted(glob.glob('*.txt'))
        if not args.files:
            print("当前目录没有找到任何 .txt 文件。")
            return

    # 定义不同的线型样式（循环使用）
    linestyles = ['-', '--', '-.', ':', (0, (3, 1, 1, 1)), (0, (5, 2, 1, 2))]
    
    # 使用matplotlib的Tab10颜色映射，提供10种不同的颜色
    colors = plt.cm.tab10(np.linspace(0, 1, 10))
    # 或者手动定义更多颜色
    # colors = ['blue', 'red', 'green', 'orange', 'purple', 'brown', 'pink', 'gray', 'olive', 'cyan',
    #          'magenta', 'yellow', 'black', 'darkblue', 'darkred', 'darkgreen', 'darkorange', 'indigo']

    # 先读取所有数据以确定全局 xmin（用于让每条曲线从最左侧开始）
    all_data = []
    for f in args.files:
        if not os.path.exists(f):
            print("跳过不存在的文件:", f)
            continue
        xs, ys = read_cdf_file(f)
        if xs is None:
            print("跳过空或无法解析的文件:", f)
            continue
        all_data.append((f, xs, ys))

    if len(all_data) == 0:
        print("没有可绘制的数据。")
        return

    # 全局 xmin/xmax（对数轴时保证 xmin 为最小正值）
    xmin_vals = []
    xmax_vals = []
    positive_mins = []
    for (_f, xs, _ys) in all_data:
        xmin_vals.append(xs.min())
        xmax_vals.append(xs.max())
        pos = xs[xs > 0]
        if pos.size > 0:
            positive_mins.append(pos.min())
    global_xmin = min(xmin_vals)
    global_xmax = max(xmax_vals)
    if args.logx:
        if len(positive_mins) > 0:
            global_xmin = min(positive_mins)
        else:
            global_xmin = 1e-9

    plt.figure(figsize=(10, 6))  # 稍微增大图像以便更好地显示多条曲线
    
    for idx, (f, xs, ys) in enumerate(all_data):
        xs_plot, ys_plot = smooth_cdf(xs, ys, npoints=args.smooth, ma_window=args.ma, logx=args.logx)

        # 如果曲线起点大于全局 xmin，则在开头插入 (global_xmin, 0)
        # 对数轴时保证插入点为正
        prepend_x = global_xmin
        if args.logx and prepend_x <= 0:
            prepend_x = max(1e-9, np.min(xs_plot[xs_plot > 0]) if np.any(xs_plot > 0) else 1e-9)

        if xs_plot[0] > prepend_x:
            xs_plot = np.concatenate(([prepend_x], xs_plot))
            ys_plot = np.concatenate(([0.0], ys_plot))

        # 为每条曲线分配不同的线型和颜色
        linestyle = linestyles[idx % len(linestyles)]
        color = colors[idx % len(colors)]

        # 绘图：插值/平滑时用平滑线，否则用阶梯图
        if args.smooth > 0 or args.ma > 1:
            plt.plot(xs_plot, ys_plot, label=os.path.basename(f), 
                    linewidth=args.linewidth, linestyle=linestyle, color=color)
        else:
            plt.step(xs_plot, ys_plot, where='post', label=os.path.basename(f), 
                    linewidth=args.linewidth, linestyle=linestyle, color=color)

    plt.xlabel(args.xlabel, fontsize=12)
    plt.ylabel(args.ylabel, fontsize=12)
    plt.xlim(left=global_xmin)  # 强制 x 轴从全局 xmin 开始
    plt.ylim(0, 1.02)
    plt.grid(True, linestyle='--', alpha=0.3)
    if args.logx:
        plt.xscale('log')
    
    # 改进图例显示
    plt.legend(loc='lower right', fontsize='small', frameon=True, fancybox=True, shadow=True)
    plt.tight_layout()
    plt.savefig(args.out, dpi=300, bbox_inches='tight')  # 提高输出质量
    print("已保存:", args.out)

if __name__ == '__main__':
    main()