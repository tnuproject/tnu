#!/bin/sh
echo POSIX shell path: $0
echo first argument: $1
THING=works
echo assignment: $THING
pwd
if exists /etc/os-release {
echo found os-release
}
for file in /etc/* {
echo etc-entry: $file
}
echo last status: $?
exit 0
