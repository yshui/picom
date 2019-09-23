#!/bin/sh
compton=$(realpath $1)
cd $(dirname $0)

./run_one_test.sh $compton configs/empty.conf testcases/basic.py
