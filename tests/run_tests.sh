#!/bin/sh
set -e
exe=$(realpath $1)
cd $(dirname $0)

./run_one_test.sh $exe configs/empty.conf testcases/basic.py
./run_one_test.sh $exe configs/issue239.conf testcases/issue239.py
./run_one_test.sh $exe configs/issue239_2.conf testcases/issue239_2.py
./run_one_test.sh $exe configs/issue239_3.conf testcases/issue239_3.py
./run_one_test.sh $exe configs/issue239_3.conf testcases/issue239_3_norefresh.py
<<<<<<< HEAD
./run_one_test.sh $exe configs/issue314.conf testcases/issue314.py
./run_one_test.sh $exe configs/issue314.conf testcases/issue314_2.py
./run_one_test.sh $exe configs/issue314.conf testcases/issue314_3.py
./run_one_test.sh $exe /dev/null testcases/issue299.py
./run_one_test.sh $exe configs/issue465.conf testcases/issue465.py
./run_one_test.sh $exe configs/clear_shadow_unredirected.conf testcases/clear_shadow_unredirected.py
./run_one_test.sh $exe configs/clear_shadow_unredirected.conf testcases/redirect_when_unmapped_window_has_shadow.py
./run_one_test.sh $exe configs/issue394.conf testcases/issue394.py
./run_one_test.sh $exe configs/issue239.conf testcases/issue525.py
=======
>>>>>>> e3c19cd7d1108d114552267f302548c113278d45
