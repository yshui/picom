#!/bin/sh
set -e
exe=$(realpath $1)
cd $(dirname $0)

./run_one_test.sh $exe configs/empty.conf testcases/basic.py
./run_one_test.sh $exe configs/issue239.conf testcases/issue239.py
./run_one_test.sh $exe configs/issue239_2.conf testcases/issue239_2.py
./run_one_test.sh $exe configs/issue239_3.conf testcases/issue239_3.py
./run_one_test.sh $exe configs/issue239_3.conf testcases/issue239_3_norefresh.py
