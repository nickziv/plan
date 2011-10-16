#include <stdlib.h>
#include <unistd.h>


void atomic_read(int fd, void *buf, size_t sz)
{
	int total_read = 0;
	int red = 0;
	do {
		red = read(fd, buf, sz);
		total_read += red;
	} while (total_read < sz && red != -1);
}


void atomic_write(int fd, void *buf, size_t sz)
{

	int total_written = 0;
	int written = 0;
	do {
		written = write(fd, buf, sz);
		total_written += written;
	} while (total_written < sz && written != -1);
}
