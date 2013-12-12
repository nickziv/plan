plan:
	dtrace -h -s plan_probes.d
	gcc -c plan_manip.c
	gcc -c plan_main.c
	gcc -c plan_atomic.c
	dtrace -G -32 -s plan_probes.d plan_manip.o plan_main.o plan_atomic.o
	gcc -o plan plan_main.o plan_manip.o plan_atomic.o plan_probes.o -lumem -ldtrace

clean:
	rm plan_main.o
	rm plan_manip.o
	rm plan_probes.o
	rm plan_atomic.o
	rm plan
