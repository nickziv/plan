pid$target::umem_cache_create:entry,
pid$target::umem_cache_alloc:entry,
pid$target::umem_free:entry,
pid$target::umem_cache_destroy:entry,
pid$target:plan::entry
{
	self->ts = timestamp;
	trace(0);
	funcarr[probefunc] = self->ts;
}

pid$target::umem_cache_create:return,
pid$target::umem_cache_alloc:return,
pid$target::umem_free:return,
pid$target::umem_cache_destroy:entry,
pid$target:plan::return
{
	trace(timestamp - funcarr[probefunc]);
}

vminfo:::
/pid == $target/
{
	trace(probename);
}

plan$target:::vmem_xalloc
{
	trace(arg0);
}
