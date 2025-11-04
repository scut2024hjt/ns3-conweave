#!/bin/bash

python3 run.py --lb fecmp --pfc 1 --irn 0 --simul_time 0.1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb drill --pfc 1 --irn 0 --simul_time 0.1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb letflow --pfc 1 --irn 0 --simul_time 0.1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb conga --pfc 1 --irn 0 --simul_time 0.1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb conweave --pfc 1 --irn 0 --simul_time 0.1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb proteus --pfc 1 --irn 0 --simul_time 0.1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&

python3 run.py --lb fecmp --pfc 1 --irn 0 --simul_time 0.5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb drill --pfc 1 --irn 0 --simul_time 0.5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb letflow --pfc 1 --irn 0 --simul_time 0.5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb conga --pfc 1 --irn 0 --simul_time 0.5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb conweave --pfc 1 --irn 0 --simul_time 0.5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb proteus --pfc 1 --irn 0 --simul_time 0.5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&

python3 run.py --lb fecmp --pfc 1 --irn 0 --simul_time 1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb drill --pfc 1 --irn 0 --simul_time 1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb letflow --pfc 1 --irn 0 --simul_time 1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb conga --pfc 1 --irn 0 --simul_time 1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb conweave --pfc 1 --irn 0 --simul_time 1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb proteus --pfc 1 --irn 0 --simul_time 1 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&

python3 run.py --lb fecmp --pfc 1 --irn 0 --simul_time 5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb drill --pfc 1 --irn 0 --simul_time 5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb letflow --pfc 1 --irn 0 --simul_time 5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb conga --pfc 1 --irn 0 --simul_time 5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb conweave --pfc 1 --irn 0 --simul_time 5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&
python3 run.py --lb proteus --pfc 1 --irn 0 --simul_time 5 --netload 50 --topo leaf_spine_128_100G_OS2 --cdf transformer_moe_cdf 2>&1 > /dev/null&