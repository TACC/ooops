#!/bin/bash

ratio=0.95
cat $1 | awk '{print $2}' | sort -n > 1
nline=`wc -l 1 | awk '{print $1}'`
npicked=`echo "$nline * $ratio  / 1" | bc`
head -n $npicked 1 | tail -n 1
