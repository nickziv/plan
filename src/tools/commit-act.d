plan$target:::set_dur
{
	printf("%s dur:%d\n", copyinstr(arg0), arg1);
}

plan$target:::commit_act
{
	printf("%s time:%d dur:%d\n", copyinstr(arg0), arg1, arg2);
}

plan$target:::read_act
{
	printf("%s time:%d dur:%d\n", copyinstr(arg0), arg1, arg2);
}
