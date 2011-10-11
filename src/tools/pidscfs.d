pid$target:plan::entry,
pid$target:plan::return
{

}

fsinfo:::open,
fsinfo:::close,
fsinfo:::read,
fsinfo:::write,
fsinfo:::seek
/pid == $target/
{
	printf("[%d] %-26s\n", args[1], args[0]->fi_pathname);
}
