pid$target:::entry
{
	self->ts = timestamp;
	funcarr[probemod, probefunc] = self->ts;
}

pid$target:::return
{
	@[probemod, probefunc] = sum(timestamp - funcarr[probemod, probefunc]);
}
