# ooops
OOOPS, short for Optimal Overloaded IO Protection System, is an innovative IO workload managing system that optimally throttles the IO workload from the users' side. Thank Si Liu for coming up the name of this tool! <br><br>
You can find [our paper](https://github.com/TACC/ooops/raw/master/OOOPS_2018.pdf) and [our slides](https://github.com/TACC/ooops/raw/master/OOOPS_HUST_2018_final.pdf) for details. Please feel free to contact [Lei Huang](https://www.tacc.utexas.edu/about/directory/lei-huang) for questions or suggestions. Thank you!

Centralized file system shared by large number of users has been a week point of modern super computers for a while since there is no IO resource provisioning enforced. A small number of nodes could satuate the whole file system easily. Overloading file system could result in global filesystem performance degradation and even unresponsiveness for all users. ooops is designed to proactively protect file system and throttle the nodes with excessive I/O. With some modification, ooops can support user IO resource provisioning to enforce fair sharing. ooops is running on every node, so it can scale up to any arbitrary size of super computers. There is NO need for users to change anything on their side. The work for administrator to deploy ooops is also trivial. 

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

To modify configurations, you can either change config or dynamically change the setting only on given nodes with set_io_param. Dynamically throttling with set_io_param works well without interrupting users' applications. 
`set_io_param server_idx t_open max_open_freq t_stat max_stat_freq`<br>
`or`<br>
`set_io_param server_idx [ low / medium / high / unlimit ]`

Example to use ooops to control the level of IO from one compute node. <br>
![Alt text](ooops_levels.png?raw=true "IO under various settings")

Example to use ooops to adjust the level of IO on-fly when a job is running.
![Alt text](change_io_on_fly.jpg?raw=true "Adjust IO level on-fly")

