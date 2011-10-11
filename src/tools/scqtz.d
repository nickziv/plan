syscall::read:entry,
syscall::write:entry,
syscall::openat:entry,
syscall::close:entry
/pid == $target/
{
	self->ts = timestamp;
}

syscall::read:return,
syscall::write:return,
syscall::openat:return,
syscall::close:return
/pid == $target/
{
	@[probefunc, ustack()] = count();
}
