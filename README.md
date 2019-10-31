# ooops
ooops is an innovative IO workload managing system that optimally throttles the IO workload from the users' side. <br>
You can find 
[our paper]: https://github.com/TACC/ooops/blob/master/OOOPS_2018.pdf "our paper"
and 
[slides]: https://github.com/TACC/ooops/blob/master/OOOPS_HUST_2018_final.pdf "our slides"



To compile,<br> 
`make all`<br>

To test how ooops works, <br>
`export IO_LIMIT_CONFIG=`pwd`/config`<br>
`export LD_PRELOAD=`pwd`/wrapper.so`<br>

