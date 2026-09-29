// SPDX-License-Identifier: GPL-3.0+
/*
 * Copyright (C) 2026 Tal Zussman
 *
 * Check that a RWF_ATOMIC direct write to a block device is all or nothing
 * when only part of its buffer can be pinned.
 *
 * Fill two pages of the device with one byte value, then issue a pwritev2()
 * with RWF_ATOMIC of a two-page buffer holding another value whose last page
 * is PROT_NONE, so that pinning the buffer stops one page short. The direct
 * I/O path must fail the write with EINVAL rather than submit what it pinned.
 *
 * Read the range back with O_DIRECT and count the bytes of each value, once
 * before the atomic write to check the baseline and once after it. The write
 * has to fail with EINVAL and leave only old bytes on the device.
 *
 * The device has to support atomic writes of two pages, which is checked
 * with statx(). The first two pages of <blockdev> are overwritten.
 *
 * usage: atomic-write-torn <blockdev>
 *
 * exit:  0 = the write failed with EINVAL and the device is unchanged
 *        1 = setup error
 *        2 = the write was torn
 *        3 = the write did not fail with EINVAL, or went through
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef RWF_ATOMIC
#define RWF_ATOMIC	0x00000040
#endif

#ifndef STATX_WRITE_ATOMIC
#define STATX_WRITE_ATOMIC	0x00010000U
#endif
#ifndef STATX_ATTR_WRITE_ATOMIC
#define STATX_ATTR_WRITE_ATOMIC	0x00400000
#endif

/*
 * The atomic write fields of struct statx are only in glibc 2.43 and later,
 * so use a local copy of the kernel's layout and call the syscall directly.
 */
struct atomic_statx {
	uint32_t stx_mask;
	uint32_t stx_blksize;
	uint64_t stx_attributes;
	uint32_t stx_nlink;
	uint32_t stx_uid;
	uint32_t stx_gid;
	uint16_t stx_mode;
	uint16_t __spare0[1];
	uint64_t stx_ino;
	uint64_t stx_size;
	uint64_t stx_blocks;
	uint64_t stx_attributes_mask;
	uint64_t stx_times[8];	/* stx_atime, stx_btime, stx_ctime, stx_mtime */
	uint32_t stx_rdev_major;
	uint32_t stx_rdev_minor;
	uint32_t stx_dev_major;
	uint32_t stx_dev_minor;
	uint64_t stx_mnt_id;
	uint32_t stx_dio_mem_align;
	uint32_t stx_dio_offset_align;
	uint64_t stx_subvol;
	uint32_t stx_atomic_write_unit_min;
	uint32_t stx_atomic_write_unit_max;
	uint32_t stx_atomic_write_segments_max;
	uint32_t stx_dio_read_offset_align;
	uint32_t stx_atomic_write_unit_max_opt;
	uint32_t __spare2[1];
	uint64_t __spare3[8];
};

_Static_assert(sizeof(struct atomic_statx) == 256, "struct statx is 256 bytes");

#define EXIT_TORN	2
#define EXIT_UNEXPECTED	3

#define OLD	'A'
#define NEW	'B'

/* Does the device support an atomic write of len bytes? */
static bool can_write_atomically(int fd, size_t len)
{
	struct atomic_statx stx = { 0 };

	if (syscall(SYS_statx, fd, "", AT_EMPTY_PATH, STATX_WRITE_ATOMIC,
		    &stx)) {
		perror("statx");
		return false;
	}
	if (!(stx.stx_mask & STATX_WRITE_ATOMIC) ||
	    !(stx.stx_attributes & STATX_ATTR_WRITE_ATOMIC)) {
		fprintf(stderr, "device does not support atomic writes\n");
		return false;
	}
	printf("atomic write unit min %u, max %u\n",
	       stx.stx_atomic_write_unit_min, stx.stx_atomic_write_unit_max);
	if (len < stx.stx_atomic_write_unit_min ||
	    len > stx.stx_atomic_write_unit_max) {
		fprintf(stderr, "device cannot write %zu bytes atomically\n",
			len);
		return false;
	}
	return true;
}

/* Read the range back past the page cache and count the bytes of each value. */
static int count_bytes(int fd, char *buf, size_t len, size_t *old_bytes,
		       size_t *new_bytes)
{
	size_t i;

	if (pread(fd, buf, len, 0) != (ssize_t)len) {
		perror("pread");
		return -1;
	}

	*old_bytes = *new_bytes = 0;
	for (i = 0; i < len; i++) {
		if (buf[i] == OLD)
			(*old_bytes)++;
		else if (buf[i] == NEW)
			(*new_bytes)++;
	}
	printf("on disk: %zu old bytes, %zu new bytes, %zu other\n",
	       *old_bytes, *new_bytes, len - *old_bytes - *new_bytes);
	return 0;
}

int main(int argc, char **argv)
{
	size_t old_bytes, new_bytes;
	size_t len, pgsz;
	struct iovec iov;
	char *buf;
	ssize_t ret;
	int fd, err, status = 1;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <blockdev>\n", argv[0]);
		return 1;
	}

	pgsz = sysconf(_SC_PAGESIZE);
	len = 2 * pgsz;

	fd = open(argv[1], O_RDWR | O_DIRECT);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (!can_write_atomically(fd, len))
		goto out_close;

	buf = mmap(NULL, len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) {
		perror("mmap");
		goto out_close;
	}

	/* Write the old contents and check that they read back. */
	memset(buf, OLD, len);
	if (pwrite(fd, buf, len, 0) != (ssize_t)len) {
		perror("pwrite");
		goto out_unmap;
	}
	if (count_bytes(fd, buf, len, &old_bytes, &new_bytes))
		goto out_unmap;
	if (old_bytes != len) {
		fprintf(stderr, "old contents did not read back\n");
		goto out_unmap;
	}

	/* Fill in the new contents and make the last page unreadable. */
	memset(buf, NEW, len);
	if (mprotect(buf + len - pgsz, pgsz, PROT_NONE)) {
		perror("mprotect");
		goto out_unmap;
	}

	iov.iov_base = buf;
	iov.iov_len = len;
	ret = pwritev2(fd, &iov, 1, 0, RWF_ATOMIC);
	err = errno;
	if (ret < 0)
		printf("pwritev2(RWF_ATOMIC) failed: %s\n", strerror(err));
	else
		printf("pwritev2(RWF_ATOMIC) returned %zd of %zu\n", ret, len);

	/* Make the last page readable again and reuse the buffer. */
	if (mprotect(buf + len - pgsz, pgsz, PROT_READ | PROT_WRITE)) {
		perror("mprotect");
		goto out_unmap;
	}
	if (count_bytes(fd, buf, len, &old_bytes, &new_bytes))
		goto out_unmap;

	if (old_bytes != len && new_bytes != len) {
		printf("atomic write was torn: %zu of %zu bytes written\n",
		       new_bytes, len);
		status = EXIT_TORN;
	} else if (ret >= 0 || err != EINVAL) {
		printf("atomic write did not fail with EINVAL\n");
		status = EXIT_UNEXPECTED;
	} else if (new_bytes) {
		printf("atomic write went through despite the unreadable page\n");
		status = EXIT_UNEXPECTED;
	} else {
		printf("atomic write failed with EINVAL and left the device unchanged\n");
		status = 0;
	}

out_unmap:
	munmap(buf, len);
out_close:
	close(fd);
	return status;
}
