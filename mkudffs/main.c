/*
 * main.c
 *
 * Copyright (c) 2001-2002  Ben Fennema <bfennema@falcon.csc.calpoly.edu>
 * Copyright (c) 2014       Pali Rohár <pali.rohar@gmail.com>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

/**
 * @file
 * mkudffs main program and I/O functions
 */

#include "config.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/fd.h>

#include "mkudffs.h"
#include "defaults.h"
#include "options.h"

static int valid_offset(int fd, off_t offset)
{
	char ch;

	if (lseek(fd, offset, SEEK_SET) < 0)
		return 0;
	if (read(fd, &ch, 1) < 1)
		return 0;
	return 1;
}

uint32_t get_blocks(int fd, int blocksize, uint32_t opt_blocks)
{
	uint64_t blocks;
#ifdef BLKGETSIZE64
	uint64_t size64;
#endif
#ifdef BLKGETSIZE
	long size;
#endif
#ifdef FDGETPRM
	struct floppy_struct this_floppy;
#endif
	struct stat buf;

	if (opt_blocks)
		return opt_blocks;

#ifdef BLKGETSIZE64
	if (ioctl(fd, BLKGETSIZE64, &size64) >= 0)
		blocks = size64 / blocksize;
	else
#endif
#ifdef BLKGETSIZE
	if (ioctl(fd, BLKGETSIZE, &size) >= 0)
		blocks = size / (blocksize / 512);
	else
#endif
#ifdef FDGETPRM
	if (ioctl(fd, FDGETPRM, &this_floppy) >= 0)
		blocks = this_floppy.size / (blocksize / 512);
	else
#endif
	if (fstat(fd, &buf) == 0 && S_ISREG(buf.st_mode))
		blocks = buf.st_size / blocksize;
	else
	{
		off_t high, low;

		for (low=0, high = 1024; valid_offset(fd, high); high *= 2)
			low = high;
		while (low < high - 1)
		{
			const off_t mid = (low + high) / 2;

			if (valid_offset(fd, mid))
				low = mid;
			else
				high = mid;
		}

		valid_offset(fd, 0);
		blocks = (low + 1) / blocksize;
	}

	if (blocks > UINT32_MAX)
	{
		fprintf(stderr, "mkudffs: Warning: Disk is too big, using only %lu blocks\n", (unsigned long int)UINT32_MAX);
		return UINT32_MAX;
	}

	return blocks;
}

static void detect_blocksize(int fd, struct udf_disc *disc, int *blocksize)
{
	int size;
	uint16_t bs;

#ifdef BLKSSZGET
	if (ioctl(fd, BLKSSZGET, &size) != 0)
		return;

	disc->blkssz = size;

	if (*blocksize != -1)
		return;

	disc->blocksize = size;
	for (bs=512,disc->blocksize_bits=9; disc->blocksize_bits<13; disc->blocksize_bits++,bs<<=1)
	{
		if (disc->blocksize == bs)
			break;
	}
	if (disc->blocksize_bits == 13)
	{
		disc->blocksize = 2048;
		disc->blocksize_bits = 11;
		*blocksize = -1;
	}
	else
		*blocksize = disc->blocksize;
	disc->udf_lvd[0]->logicalBlockSize = cpu_to_le32(disc->blocksize);
#endif
}

int write_func(struct udf_disc *disc, struct udf_extent *ext)
{
	static char *buffer = NULL;
	static size_t bufferlen = 0;
	int fd = *(int *)disc->write_data;
	ssize_t length, offset;
	uint32_t blocks;
	struct udf_desc *desc;
	struct udf_data *data;

	if (buffer == NULL)
	{
		bufferlen = disc->blocksize;
		buffer = calloc(bufferlen, 1);
	}

	if (!(ext->space_type & (USPACE|RESERVED)))
	{
		desc = ext->head;
		while (desc != NULL)
		{
			if (lseek(fd, (off_t)(ext->start + desc->offset) << disc->blocksize_bits, SEEK_SET) < 0)
				return -1;
			data = desc->data;
			offset = 0;
			while (data != NULL)
			{
				if (data->length + offset > bufferlen)
				{
					bufferlen = (data->length + offset + disc->blocksize - 1) & ~(disc->blocksize - 1);
					buffer = realloc(buffer, bufferlen);
				}
				memcpy(buffer + offset, data->buffer, data->length);
				offset += data->length;
				data = data->next;
			}
			length = (offset + disc->blocksize - 1) & ~(disc->blocksize - 1);
			if (offset != length)
				memset(buffer + offset, 0x00, length - offset);
			if (write(fd, buffer, length) != length)
				return -1;
			desc = desc->next;
		}
	}
	else if (!(disc->flags & FLAG_BOOTAREA_PRESERVE))
	{
		length = disc->blocksize;
		blocks = ext->blocks;
		memset(buffer, 0, length);
		if (lseek(fd, (off_t)(ext->start) << disc->blocksize_bits, SEEK_SET) < 0)
			return -1;
		while (blocks-- > 0)
		{
			if (write(fd, buffer, length) != length)
				return -1;
		}
	}
	return 0;
}
	
int main(int argc, char *argv[])
{
	struct udf_disc	disc;
	struct stat stat;
	char filename[NAME_MAX];
	char buf[128*3];
	int fd;
	int blocksize = -1;
	int media;
	size_t len;

	setlocale(LC_CTYPE, "");

	udf_init_disc(&disc);
	parse_args(argc, argv, &disc, filename, &blocksize, &media);
	fd = open(filename, O_RDONLY);
	if (fd < 0)
	{
		if (errno != ENOENT)
		{
			fprintf(stderr, "mkudffs: Error: Cannot open device '%s': %s\n", filename, strerror(errno));
			exit(1);
		}

		if (!disc.blocks)
		{
			fprintf(stderr, "mkudffs: Error: Cannot create new file '%s': block-count was not specified\n", filename);
			exit(1);
		}

		// Create new file disk image
		fd = open(filename, O_RDWR | O_CREAT | O_EXCL, 0660);
		if (fd < 0)
		{
			fprintf(stderr, "mkudffs: Error: Cannot create new file '%s': %s\n", filename, strerror(errno));
			exit(1);
		}
	}
	else
	{
		int fd2;
		int flags2;
		char filename2[NAME_MAX];
		const char *error;

		if (fstat(fd, &stat) != 0)
		{
			fprintf(stderr, "mkudffs: Error: Cannot stat device '%s': %s\n", filename, strerror(errno));
			exit(1);
		}

		flags2 = O_RDWR;
		snprintf(filename2, sizeof(filename2), "/proc/self/fd/%d", fd);

		// Re-open block device with O_EXCL mode which fails when device is already mounted
		if (S_ISBLK(stat.st_mode))
			flags2 |= O_EXCL;

		fd2 = open(filename2, flags2);
		if (fd2 < 0)
		{
			if (errno != ENOENT)
			{
				error = (errno != EBUSY) ? strerror(errno) : "Device is mounted or mkudffs is already running";
				fprintf(stderr, "mkudffs: Error: Cannot open device '%s': %s\n", filename, error);
				exit(1);
			}

			// Fallback to orignal filename when /proc is not available, but this introduce race condition between stat and open
			fd2 = open(filename, flags2);
			if (fd2 < 0)
			{
				error = (errno != EBUSY) ? strerror(errno) : "Device is mounted or mkudffs is already running";
				fprintf(stderr, "mkudffs: Error: Cannot open device '%s': %s\n", filename, error);
				exit(1);
			}
		}

		close(fd);
		fd = fd2;
	}

	detect_blocksize(fd, &disc, &blocksize);

	if (blocksize == -1 && media == MEDIA_TYPE_HD) {
		disc.blocksize = 512;
		disc.blocksize_bits = 9;
		disc.udf_lvd[0]->logicalBlockSize = cpu_to_le32(disc.blocksize);
	}

	disc.blocks = get_blocks(fd, disc.blocksize, disc.blocks);
	disc.head->blocks = disc.blocks;
	disc.write = write_func;
	disc.write_data = &fd;

	printf("filename=%s\n", filename);

	memset(buf, 0, sizeof(buf));
	len = decode_string(&disc, disc.udf_lvd[0]->logicalVolIdent, buf, 128, sizeof(buf));
	printf("label=%s\n", buf);

	memset(buf, 0, sizeof(buf));
	len = gen_uuid_from_vol_set_ident(buf, disc.udf_pvd[0]->volSetIdent, 128);
	printf("uuid=%s\n", buf);

	printf("blocksize=%u\n", (unsigned int)disc.blocksize);
	printf("blocks=%lu\n", (unsigned long int)disc.blocks);
	printf("udfrev=%d.%02d\n", (int)(disc.udf_rev >> 8), (int)(disc.udf_rev & 0xFF));

	if (((disc.flags & FLAG_BRIDGE) && disc.blocks < 513) || disc.blocks < 281)
	{
		fprintf(stderr, "mkudffs: Error: Not enough blocks on device '%s'\n", filename);
		exit(1);
	}

	if ((disc.flags & FLAG_BOOTAREA_MBR) && (((uint64_t)disc.blocks) << disc.blocksize_bits)/disc.blkssz >= UINT32_MAX)
	{
		fprintf(stderr, "mkudffs: Error: Cannot create MBR on disc larger then 2^32 logical sectors\n");
		exit(1);
	}

	split_space(&disc);

	setup_mbr(&disc);
	setup_vrs(&disc);
	setup_anchor(&disc);
	setup_partition(&disc);
	setup_vds(&disc);

	dump_space(&disc);

	if (len == (size_t)-1)
		fprintf(stderr, "mkudffs: Warning: Volume Set Identifier must be at least 8 characters long\n");
	else if (len < 16)
		fprintf(stderr, "mkudffs: Warning: First 16 characters of Volume Set Identifier are not hexadecimal lowercase digits\nmkudffs: Warning: This would cause problems for UDF uuid\n");

	if (write_disc(&disc) < 0) {
		fprintf(stderr, "mkudffs: Error: Cannot write to device '%s': %s\n", filename, strerror(errno));
		return 1;
	}

	return 0;
}
