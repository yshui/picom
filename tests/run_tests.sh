#!/bin/sh
set -e
compton=$(realpath $1)
cd $(dirname $0)

./run_one_test.sh $compton configs/empty.conf testcases/basic.py
./run_one_test.sh $compton configs/issue239.conf testcases/issue239.py
./run_one_test.sh $compton configs/issue239_2.conf testcases/issue239_2.py
