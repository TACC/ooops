# ooops
ooops is an innovative IO workload managing system that optimally throttles the IO workload from the users' side. <br>
You can find [our paper](https://github.com/TACC/ooops/raw/master/OOOPS_2018.pdf) and [our slides](https://github.com/TACC/ooops/raw/master/OOOPS_HUST_2018_final.pdf) for details. Please feel free to contact [Lei Huang](https://www.tacc.utexas.edu/about/directory/lei-huang) for questions or suggestions. Thank you!

Centralized file system shared by large number of users has been a week point of modern super computers for a while since there is no IO resource provisioning enforced.A small number of nodes could satuate file system easily. Overloading file system could result in global filesystem performance degradation and even unresponsiveness. ooops is designed to proactively protect file system and throttle the nodes with excessive I/O. With some modification, ooops can support user IO resource provisioning. ooops is running on every node, so it can scale up to any arbitrary size of super computers. 

To compile,<br> 
`make all`<br>

To measure the time needed for open() and stat() function calls on your file system<br>
Usage: <br>
`./t_open_stat fs_path n_hours` <br>
`./cal_threshhold.sh t_log_open.txt` <br>
`./cal_threshhold.sh t_log_stat.txt` <br>
Update config with the parameter you get for T_THRESHOLD_OPEN_x and T_THRESHOLD_LXSTAT_x.

To deploy ooops, <br>
`export IO_LIMIT_CONFIG=${PWD}/config` <br>
`export LD_PRELOAD=${PWD}/wrapper.so` <br>
 Run your apps as usual. 


