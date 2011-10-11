/*
plan$target:::vmem_xalloc
{
	printf("[%d] %d %d -> %d\n", arg0, arg1, arg2, arg3);

}
*/

fsinfo:::read,
fsinfo:::write,
fsinfo:::seek
/pid == $target/
{
	printf("[%d] %-26s\n", args[1], args[0]->fi_pathname
		);
}
