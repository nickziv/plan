pid$target:plan:read_todo_dir:entry
{
	self->follow = 1;
}

pid$target:plan:read_todo_dir:return
{
	self->follow = 0;
}

pid$target:::entry
/self->follow == 1/
{
	trace(probemod);
}

pid$target:::return
/self->follow == 1/
{
	trace(probemod);
}
