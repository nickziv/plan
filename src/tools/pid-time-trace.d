pid$target:plan::entry
{
	self->ts = timestamp;
	trace(0);
	funcarr[probefunc] = self->ts;
}

pid$target:plan::return
{
	trace(timestamp - funcarr[probefunc]);
}

syscall:::entry
/pid == $target/
{
	self->ts = timestamp;

}

syscall:::return
/pid == $target/
{
	@[probefunc] = sum(timestamp - self->ts);

}
