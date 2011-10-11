plan:
	dtrace -h -s plan_probes.d
	cc -c plan_manip.c
	cc -c plan_main.c
	dtrace -G -32 -s plan_probes.d plan_manip.o plan_main.o
	cc -o plan plan_main.o plan_manip.o plan_probes.o -lumem -ldtrace

clean:
	rm plan_main.o
	rm plan_manip.o
	rm plan_probes.o
	rm plan
