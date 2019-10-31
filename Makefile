all: set_io_param wrapper.so test_open test_stat

wrapper.so:
	gcc -O2 -fPIC -shared -o wrapper.so wrapper.c -lrt -ldl

set_io_param: 
	gcc -O2 -o set_io_param set_io_param.c -lrt

test_open:
	gcc -O2 -o test_open test_open.c

test_stat:
	gcc -O2 -o test_stat test_stat.c

clean:
	rm set_io_param wrapper.so test_open test_stat
