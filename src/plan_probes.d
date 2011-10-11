provider plan {
	probe realloc_loop(void *);
	probe commit_acts_loop(void *);
	probe commit_act(char *, int, size_t);
	probe precommit_dur(char *, size_t);
	probe read_act(char *, int, size_t);
	probe ntimes(int);
	probe read_todo(char *, int);
	probe vmem_xalloc(void *, size_t, void *, void *);
	probe vmem_create(void *);
	probe gothere(int);
	probe act_ptr(void *);
	probe parse_dur(size_t);
	probe do_dur(void *, size_t);
	probe set_dur(char*, size_t);
};
