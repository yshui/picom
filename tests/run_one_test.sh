#!/usr/bin/env bash
set -xe
if [ -z $DISPLAY ]; then
	exec xvfb-run -s "+extension composite" -a $0 $1 $2 $3
fi

echo "Running test $2"

picom_exe=$1
config=$2
test_script=$3

function test_with_backend() {
	backend=$1
	# TODO keep the log file, and parse it to see if test is successful
	($picom_exe --dbus --backend $backend --log-level=debug --log-file=$PWD/log --config=$config) &
	main_pid=$!
	$test_script

	kill -INT $main_pid || true
	cat log
	rm log
	wait $main_pid
}

test_with_backend dummy
test_with_backend xrender
test_with_backend glx
# test_with_backend egl

