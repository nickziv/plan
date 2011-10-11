plan$target:::vmem_xalloc
{
	printf("[%d] %d > %d < %d\n", arg0, arg2, arg1, arg3);

}

syscall::read:entry,
syscall::write:entry
/pid == $target/
{

	self->path = fds[0].fi_pathname;
	self->off = fds[0].fi_offset;
}

syscall::read:return,
syscall::write:return
/pid == $target/
{
	printf("[%d] %-26s %d\n", arg1, self->path,
		self->off);
}
