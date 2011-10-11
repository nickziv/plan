pid$target::umem_alloc:entry,
pid$target::umem_free:entry,
pid$target::umem_cache_alloc:entry,
pid$target::umem_cache_free:entry
{
	@[probefunc] = count();
}
