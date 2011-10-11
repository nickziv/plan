pid$target::realloc_acts:entry
{
	self->re = 1;
}

pid$target:libumem.so.1::entry,
pid$target:libumem.so.1::return
/self->re == 1/
{
	trace(probemod);
}

pid$target::realloc_acts:return
{
	self->re = 0;
}
