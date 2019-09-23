#!/bin/sh
set -x
if [ -z $DISPLAY ]; then
	exec xvfb-run -s "+extension composite" -a $0 $1 $2 $3
fi

echo "Running test $2"

# TODO keep the log file, and parse it to see if test is successful
($1 --experimental-backends --backend dummy --log-level=debug --log-file=$PWD/log --config=$2) &
compton_pid=$!
$3

kill -INT $compton_pid
wait $compton_pid
cat log
rm log


