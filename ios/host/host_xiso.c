/*
HOST_XISO.C

The maps folder out of an Xbox disc image, for the iOS port: the player
copies their own disc image of Halo: Combat Evolved (.iso or .xiso) into
the app's folder with the Files app, and the first start copies maps/ out
of it (OpenCE's desktop builds do the same in port/linux/src/xiso.c, which
the iOS host, being 64-bit, cannot compile with the game's headers).

The Xbox file system (XDVDFS): a volume descriptor at sector 32 of the game
partition names the root directory's sector and size; a directory is a
binary tree of entries (left and right subtree offsets in 4-byte units,
start sector, size, attributes, name). The partition starts at the image's
beginning in an xiso, and further in a full disc dump (redump's images of
the original discs: XGD1 at 0x18300000).
*/

#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SECTOR 2048ull
#define MAGIC "MICROSOFT*XBOX*MEDIA"

struct image
{
	int fd;
	uint64_t partition;
	char *error;
	size_t error_size;
	host_xiso_progress progress;
	void *context;
	unsigned long long done, total;
};

static int read_at(struct image *image, uint64_t offset, void *buffer, size_t size)
{
	size_t got = 0;

	while (got < size)
	{
		ssize_t result = pread(image->fd, (char *)buffer + got, size - got, (off_t)(image->partition + offset + got));

		if (result <= 0)
			return 0;
		got += (size_t)result;
	}
	return 1;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

typedef int (*entry_proc)(struct image *image, const char *name, uint32_t sector, uint32_t size, int directory,
	void *context);

/* every entry of a directory (its data loaded) in tree order */
static int walk(struct image *image, const uint8_t *data, uint32_t size, uint32_t offset, entry_proc proc,
	void *context, int depth)
{
	const uint8_t *entry;
	char name[256];
	uint16_t left, right;
	uint8_t length;

	if (depth > 64 || offset + 14 > size)
		return 1;
	entry = data + offset;
	left = le16(entry);
	right = le16(entry + 2);
	if (left == 0xffff && right == 0xffff)
		return 1;
	length = entry[13];
	if (offset + 14 + length > size)
		return 1;
	if (left && !walk(image, data, size, (uint32_t)left * 4, proc, context, depth + 1))
		return 0;
	memcpy(name, entry + 14, length);
	name[length] = 0;
	if (!proc(image, name, le32(entry + 4), le32(entry + 8), (entry[12] & 0x10) != 0, context))
		return 0;
	if (right && !walk(image, data, size, (uint32_t)right * 4, proc, context, depth + 1))
		return 0;
	return 1;
}

static uint8_t *load_directory(struct image *image, uint32_t sector, uint32_t size)
{
	uint8_t *data;

	if (!size || size > 64u * 1024u * 1024u)
		return NULL;
	data = malloc(size);
	if (data && !read_at(image, (uint64_t)sector * SECTOR, data, size))
	{
		free(data);
		data = NULL;
	}
	return data;
}

/* ---------- finding maps/ */

struct found
{
	uint32_t sector, size;
	int found;
};

static int find_maps(struct image *image, const char *name, uint32_t sector, uint32_t size, int directory,
	void *context)
{
	struct found *found = context;

	(void)image;
	if (directory && !strcasecmp(name, "maps"))
	{
		found->sector = sector;
		found->size = size;
		found->found = 1;
		return 0;
	}
	return 1;
}

static int add_size(struct image *image, const char *name, uint32_t sector, uint32_t size, int directory,
	void *context)
{
	(void)name;
	(void)sector;
	(void)context;
	if (!directory)
		image->total += size;
	return 1;
}

/* ---------- copying */

static int copy_file(struct image *image, const char *name, uint32_t sector, uint32_t size, int directory,
	void *context)
{
	const char *folder = context;
	char path[1200];
	static uint8_t buffer[1 << 20];
	uint64_t offset = 0;
	int out;

	if (directory)
		return 1;
	if (strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, ".."))
		return 1;
	snprintf(path, sizeof(path), "%s/%s", folder, name);
	out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0)
	{
		snprintf(image->error, image->error_size, "Cannot write %s: %s", path, strerror(errno));
		return 0;
	}
	while (offset < size)
	{
		size_t chunk = size - offset > sizeof(buffer) ? sizeof(buffer) : (size_t)(size - offset);

		if (!read_at(image, (uint64_t)sector * SECTOR + offset, buffer, chunk) || write(out, buffer, chunk) != (ssize_t)chunk)
		{
			snprintf(image->error, image->error_size, "Cannot copy %s out of the disc image (is it complete?)", name);
			close(out);
			return 0;
		}
		offset += chunk;
		image->done += chunk;
		if (image->progress)
			image->progress(image->context, image->done, image->total);
	}
	close(out);
	return 1;
}

int host_xiso_extract_maps(const char *image_path, const char *destination, host_xiso_progress progress,
	void *context, char *error, size_t error_size)
{
	static const uint64_t partitions[] = { 0, 0x18300000ull, 0x0fd90000ull, 0x02080000ull };
	struct image image = { -1, 0, error, error_size, progress, context, 0, 0 };
	uint8_t descriptor[SECTOR];
	struct found maps = { 0, 0, 0 };
	char staging[1100], final[1100];
	uint8_t *root, *folder;
	size_t index;
	int ok;

	image.fd = open(image_path, O_RDONLY);
	if (image.fd < 0)
	{
		snprintf(error, error_size, "Cannot open %s: %s", image_path, strerror(errno));
		return 0;
	}
	for (index = 0; index < sizeof(partitions) / sizeof(partitions[0]); index++)
	{
		image.partition = partitions[index];
		if (read_at(&image, 32 * SECTOR, descriptor, sizeof(descriptor)) && !memcmp(descriptor, MAGIC, 20))
			break;
	}
	if (index == sizeof(partitions) / sizeof(partitions[0]))
	{
		snprintf(error, error_size, "%s is not an Xbox disc image.", image_path);
		close(image.fd);
		return 0;
	}
	root = load_directory(&image, le32(descriptor + 20), le32(descriptor + 24));
	if (!root)
	{
		snprintf(error, error_size, "Cannot read the disc image's root folder.");
		close(image.fd);
		return 0;
	}
	walk(&image, root, le32(descriptor + 24), 0, find_maps, &maps, 0);
	free(root);
	if (!maps.found)
	{
		snprintf(error, error_size, "The disc image has no maps folder: is it Halo: Combat Evolved?");
		close(image.fd);
		return 0;
	}
	folder = load_directory(&image, maps.sector, maps.size);
	if (!folder)
	{
		snprintf(error, error_size, "Cannot read the disc image's maps folder.");
		close(image.fd);
		return 0;
	}
	walk(&image, folder, maps.size, 0, add_size, NULL, 0);
	snprintf(staging, sizeof(staging), "%s/maps.part", destination);
	snprintf(final, sizeof(final), "%s/maps", destination);
	mkdir(staging, 0755);
	ok = walk(&image, folder, maps.size, 0, copy_file, staging, 0);
	free(folder);
	close(image.fd);
	if (ok && rename(staging, final) != 0)
	{
		snprintf(error, error_size, "Cannot make the maps folder: %s", strerror(errno));
		ok = 0;
	}
	/* (a maps.part left by a failure is written over by the next try) */
	return ok;
}
