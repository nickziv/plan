pid$target::realloc_acts:entry
{
	self->re = 1;
}

pid$target:::entry,
pid$target:::return
/self->re == 1/
{
	@[probemod] = count();
}

pid$target::realloc_acts:return
{
	self->re = 0;
}
