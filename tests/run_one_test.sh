#!/bin/sh
set -xe
if [ -z $DISPLAY ]; then
	exec xvfb-run -s "+extension composite" -a $0 $1 $2 $3
fi

echo "Running test $2"

# TODO keep the log file, and parse it to see if test is successful
($1 --dbus --backend dummy --log-level=debug --log-file=$PWD/log --config=$2) &
main_pid=$!
$3

kill -INT $main_pid || true
cat log
rm log
wait $main_pid


