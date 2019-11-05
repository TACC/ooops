# ooops
ooops is an innovative IO workload managing system that optimally throttles the IO workload from the users' side. <br>
You can find [our paper](https://github.com/TACC/ooops/raw/master/OOOPS_2018.pdf) and [our slides](https://github.com/TACC/ooops/raw/master/OOOPS_HUST_2018_final.pdf) for details.

Centralized file system shared by large number of users has been a week point of modern super computers for a while since there is no IO resource provisioning enforced.A small number of nodes could satuate file system easily. Overloading file system could result in global filesystem performance degradation and even unresponsiveness. ooops is designed to proactively protect file system and throttle the nodes with excessive I/O. With some modification, ooops can support user IO resource provisioning.  
To compile,<br> 
`make all`<br>

To test how ooops works, <br>
`export IO_LIMIT_CONFIG=${PWD}/config` <br>
`export LD_PRELOAD=${PWD}/wrapper.so` <br>

