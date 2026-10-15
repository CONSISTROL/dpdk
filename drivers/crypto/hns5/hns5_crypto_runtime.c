/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Huawei Technologies Co., Ltd
 *
 * Common HugeTLB/BMU runtime, adapted from the Hi1231 OpenSSL engine. A single
 * process owns a 32 MiB file-backed HugeTLB pool (whose physical address is
 * read from /proc/self/pagemap) and drives the BMU VF that hands out DMA
 * buffers from it. Owner lock + recovery markers preserve DMA ownership: once
 * hardware has been touched, the pool is retained unless every teardown and
 * reset step completes, so a later process can never reuse live DMA memory.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "hns5_crypto_pmd_private.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "hidrv_bmu_vf_api.h"
#include "hidrv_udev.h"
#include "hidrv_eth_api.h"
#include "ndrv_bmu_vf_api.h"

#define HNS5_POOL_ENV          "HNS5_ENGINE_POOL_FILE"
#define HNS5_DEFAULT_POOL_FILE "/mnt/huge32m/hns5-engine.pool"
#define HNS5_OWNER_LOCK_PATH   "/run/lock/hns5-bmu0.owner"
#define HNS5_LEGACY_LOCK_PATH  "/run/lock/hi1231-bmu0.owner"
#define HUGETLBFS_MAGIC        0x958458f6UL
#define HUGE_DIR               "/sys/kernel/mm/hugepages/hugepages-32768kB"

#define RLOGE(stage, ...) HNS5_LOG(ERR, __VA_ARGS__)

/* ---------------------------------------------------------------- config */

static int
hns5_config_pool(const char **path)
{
	*path = getenv(HNS5_POOL_ENV);
	if (*path == NULL || **path == '\0')
		*path = HNS5_DEFAULT_POOL_FILE;
	return 0;
}

/* ---------------------------------------------------------------- hugepages */

static int
prep_error(const char *stage, const char *path)
{
	int error = errno;
	RTE_SET_USED(stage);
	RTE_SET_USED(path);
	return -error;
}

static int
parse_counter(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long number;

	if (!isdigit((unsigned char)*text))
		return -EINVAL;
	errno = 0;
	number = strtoull(text, &end, 10);
	if (errno == ERANGE)
		return -ERANGE;
	while (isspace((unsigned char)*end))
		++end;
	if (*end != '\0')
		return -EINVAL;
	*value = (uint64_t)number;
	return 0;
}

static int
read_counter(int directory, const char *name, uint64_t *value)
{
	char text[64];
	size_t used = 0;
	int rc = 0;
	int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	ssize_t count;

	if (fd < 0)
		return prep_error("hugepages.read", name);
	while (used < sizeof(text) - 1) {
		count = read(fd, text + used, sizeof(text) - 1 - used);
		if (count < 0) {
			rc = -errno;
			break;
		}
		if (count == 0)
			break;
		used += (size_t)count;
	}
	if (close(fd) != 0 && !rc)
		rc = -errno;
	text[used] = '\0';
	if (!rc)
		rc = (used == sizeof(text) - 1) ? -EOVERFLOW : parse_counter(text, value);
	return rc;
}

static int
read_availability(int directory, uint64_t *total, uint64_t *free_pages,
		  uint64_t *reserved)
{
	int rc = read_counter(directory, "nr_hugepages", total);

	if (!rc)
		rc = read_counter(directory, "free_hugepages", free_pages);
	if (!rc)
		rc = read_counter(directory, "resv_hugepages", reserved);
	return rc;
}

static int
hns5_hugepage_reserve(void)
{
	uint64_t total = 0, free_pages = 0, reserved = 0;
	int directory = open(HUGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	int rc;

	if (directory < 0)
		return prep_error("hugepages.support", HUGE_DIR);
	rc = read_availability(directory, &total, &free_pages, &reserved);
	if (rc || free_pages > reserved)
		goto out;
	if (total == UINT64_MAX) {
		rc = -EOVERFLOW;
		goto out;
	}
	uint64_t target = total + 1;
	char text[32];
	int length = snprintf(text, sizeof(text), "%" PRIu64 "\n", target);
	int fd;
	ssize_t written;

	if (length < 0 || (size_t)length >= sizeof(text)) {
		rc = -EOVERFLOW;
		goto out;
	}
	fd = openat(directory, "nr_hugepages", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		rc = prep_error("hugepages.open", "nr_hugepages");
		goto out;
	}
	written = write(fd, text, (size_t)length);
	if (written < 0)
		rc = -errno;
	else if (written != length)
		rc = -EIO;
	if (close(fd) != 0 && !rc)
		rc = -errno;
	(void)read_availability(directory, &total, &free_pages, &reserved);
	if (!rc && free_pages <= reserved)
		rc = -ENOMEM;
out:
	if (close(directory) != 0 && !rc)
		rc = -errno;
	return rc;
}

static int
validate_pool_path(const char *path)
{
	size_t length = strnlen(path, PATH_MAX);
	const char *component;

	if (path == NULL || path[0] != '/' || length == PATH_MAX || length < 2 ||
	    path[length - 1] == '/')
		return -EINVAL;
	component = path + 1;
	for (const char *p = component;; ++p) {
		if (*p && ((unsigned char)*p < 32 || (unsigned char)*p == 127))
			return -EINVAL;
		if (*p != '/' && *p != '\0')
			continue;
		if ((size_t)(p - component) > NAME_MAX)
			return -ENAMETOOLONG;
		if ((size_t)(p - component) == 1 && component[0] == '.')
			return -EINVAL;
		if ((size_t)(p - component) == 2 && component[0] == '.' &&
		    component[1] == '.')
			return -EINVAL;
		if (*p == '\0')
			break;
		component = p + 1;
	}
	return 0;
}

static int
open_directory(const char *path, int create)
{
	char components[PATH_MAX];
	char *save = NULL;
	int fd;

	memcpy(components, path, strlen(path) + 1);
	fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	for (char *part = strtok_r(components, "/", &save); part != NULL;
	     part = strtok_r(NULL, "/", &save)) {
		int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		int error;

		if (next < 0 && errno == ENOENT && create) {
			if (mkdirat(fd, part, 0700) != 0 && errno != EEXIST) {
				error = errno;
				close(fd);
				return -error;
			}
			next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		}
		error = errno;
		close(fd);
		if (next < 0)
			return -error;
		fd = next;
	}
	return fd;
}

static int
mount_id(int fd, uint64_t *id)
{
	char path[64];
	char line[256];
	FILE *info = fopen(path, "re");
	int rc;

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
	info = fopen(path, "re");
	if (info == NULL)
		return -errno;
	rc = -ENOTSUP;
	while (fgets(line, sizeof(line), info) != NULL) {
		if (strncmp(line, "mnt_id:", 7) != 0)
			continue;
		const char *number = line + 7;
		while (isspace((unsigned char)*number))
			++number;
		rc = parse_counter(number, id);
		break;
	}
	if (ferror(info))
		rc = errno ? -errno : -EIO;
	if (fclose(info) != 0 && !rc)
		rc = -errno;
	return rc;
}

static int
directory_empty(int fd)
{
	int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	DIR *directory;
	int rc = 0;
	struct dirent *entry;

	if (copy < 0)
		return -errno;
	directory = fdopendir(copy);
	if (directory == NULL) {
		int e = errno;
		close(copy);
		return -e;
	}
	errno = 0;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		rc = -ENOTEMPTY;
		break;
	}
	if (!rc && errno)
		rc = -errno;
	if (closedir(directory) != 0 && !rc)
		rc = -errno;
	return rc;
}

static int
persistent_mount_namespace(void)
{
	struct stat current, init;

	if (stat("/proc/self/ns/mnt", &current) != 0 || stat("/proc/1/ns/mnt", &init) != 0)
		return -errno;
	if (getpid() == 1 || current.st_dev != init.st_dev ||
	    current.st_ino != init.st_ino)
		return -ENOTSUP;
	return 0;
}

static int
check_hugetlbfs(int fd)
{
	struct statfs fs;

	if (fstatfs(fd, &fs) != 0)
		return -errno;
	if ((unsigned long)fs.f_type != HUGETLBFS_MAGIC)
		return -ENODEV;
	if ((uint64_t)fs.f_bsize != HNS5_POOL_BYTES)
		return -EINVAL;
	if (fs.f_flags & ST_RDONLY)
		return -EROFS;
	return 0;
}

static int
hns5_hugepage_open_directory(const char *pool_path)
{
	char directory[PATH_MAX];
	char target[64];
	struct stat st;
	int rc = validate_pool_path(pool_path);
	int fd;

	if (rc)
		return rc;
	memcpy(directory, pool_path, strlen(pool_path) + 1);
	char *slash = strrchr(directory, '/');
	const char *name = strrchr(pool_path, '/') + 1;

	if (slash == directory)
		directory[1] = '\0';
	else
		*slash = '\0';
	fd = open_directory(directory, 1);
	if (fd < 0) {
		rc = fd;
		goto fail;
	}
	if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		rc = -EEXIST;
		goto fail;
	}
	if (errno != ENOENT) {
		rc = -errno;
		goto fail;
	}
	if (fstat(fd, &st) != 0) {
		rc = -errno;
		goto fail;
	}
	if ((st.st_uid != 0 && st.st_uid != geteuid()) ||
	    ((st.st_mode & 0022) && !(st.st_mode & S_ISVTX))) {
		rc = -EACCES;
		goto fail;
	}
	rc = check_hugetlbfs(fd);
	if (!rc)
		return fd;
	if (rc != -ENODEV)
		goto fail;
	if (st.st_mode & 0022) {
		rc = -EACCES;
		goto fail;
	}
	int parent = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

	if (parent < 0) {
		rc = -errno;
		goto fail;
	}
	uint64_t here = 0, above = 0;
	struct stat parent_stat;

	rc = mount_id(fd, &here);
	if (!rc)
		rc = mount_id(parent, &above);
	if (!rc)
		rc = fstat(parent, &parent_stat) ? -errno : 0;
	close(parent);
	if (rc)
		goto fail;
	if (here != above ||
	    (st.st_dev == parent_stat.st_dev && st.st_ino == parent_stat.st_ino)) {
		rc = -EBUSY;
		goto fail;
	}
	rc = directory_empty(fd);
	if (rc)
		goto fail;
	rc = persistent_mount_namespace();
	if (rc)
		goto fail;
	snprintf(target, sizeof(target), "/proc/self/fd/%d", fd);
	if (mount("hns5", target, "hugetlbfs",
		  MS_NOSUID | MS_NODEV | MS_NOEXEC, "pagesize=32M,mode=0700") != 0) {
		rc = -errno;
		goto fail;
	}
	close(fd);
	fd = open_directory(directory, 0);
	if (fd < 0) {
		rc = fd;
		goto fail;
	}
	rc = check_hugetlbfs(fd);
	if (rc)
		goto fail;
	return fd;
fail:
	if (fd >= 0)
		close(fd);
	return rc;
}

/* ---------------------------------------------------------------- pool */

static int
legacy_owner_check(void)
{
	struct stat st;
	int fd = open(HNS5_LEGACY_LOCK_PATH, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	int rc = 0;

	if (fd < 0)
		return (errno == ENOENT) ? 0 : -errno;
	if (flock(fd, LOCK_EX | LOCK_NB) != 0 || fstat(fd, &st) != 0 ||
	    !S_ISREG(st.st_mode) || st.st_size != 0)
		rc = -EBUSY;
	close(fd);
	return rc;
}

static int
system_failure(const char *stage)
{
	RTE_SET_USED(stage);
	return -errno;
}

static int
map_pool(struct hns5_pool *pool)
{
	struct statfs fs;
	uint64_t pagemap_entry;
	uint64_t pagemap_offset;
	uint64_t pfn;
	long page_size = sysconf(_SC_PAGESIZE);
	int pagemap_fd;
	int saved_errno;
	ssize_t count;

	if (fstatfs(pool->file_fd, &fs) != 0)
		return system_failure("pool.fstatfs");
	if ((unsigned long)fs.f_type != HUGETLBFS_MAGIC ||
	    (uint64_t)fs.f_bsize != HNS5_POOL_BYTES || page_size <= 0)
		return -EINVAL;
	if (ftruncate(pool->file_fd, HNS5_POOL_BYTES) != 0)
		return system_failure("pool.ftruncate");
	if (hns5_hugepage_reserve())
		return -EIO;
	if (fallocate(pool->file_fd, 0, 0, HNS5_POOL_BYTES) != 0)
		return system_failure("pool.fallocate");
	pool->va = mmap(NULL, HNS5_POOL_BYTES, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_POPULATE, pool->file_fd, 0);
	if (pool->va == MAP_FAILED) {
		pool->va = NULL;
		return system_failure("pool.mmap");
	}
	*(volatile unsigned char *)pool->va = 0;
	pagemap_offset = ((uint64_t)(uintptr_t)pool->va / (uint64_t)page_size) *
			sizeof(pagemap_entry);
	if (pagemap_offset > INT64_MAX)
		return -EOVERFLOW;
	pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (pagemap_fd < 0)
		return system_failure("pool.pagemap.open");
	count = pread(pagemap_fd, &pagemap_entry, sizeof(pagemap_entry),
		      (off_t)pagemap_offset);
	saved_errno = errno;
	close(pagemap_fd);
	if (count != (ssize_t)sizeof(pagemap_entry))
		return (count < 0) ? -saved_errno : -EIO;
	pfn = pagemap_entry & ((UINT64_C(1) << 55) - 1);
	if ((pagemap_entry & (UINT64_C(1) << 63)) == 0 ||
	    (pagemap_entry & (UINT64_C(1) << 62)) != 0 || pfn == 0)
		return -EACCES;
	if (pfn > UINT64_MAX / (uint64_t)page_size)
		return -EOVERFLOW;
	pool->pa = pfn * (uint64_t)page_size +
		   (uintptr_t)pool->va % (uint64_t)page_size;
	if (pool->pa % HNS5_POOL_BYTES != 0 ||
	    pool->pa > (UINT64_C(1) << 48) - HNS5_POOL_BYTES)
		return -EINVAL;
	return 0;
}

static int
pool_file_identity(struct hns5_pool *pool)
{
	struct stat opened, named;
	const char *name = (pool->path != NULL) ? strrchr(pool->path, '/') : NULL;

	if (name == NULL || pool->dir_fd < 0 || pool->file_fd < 0)
		return -EINVAL;
	if (fstat(pool->file_fd, &opened) != 0 ||
	    fstatat(pool->dir_fd, name + 1, &named, AT_SYMLINK_NOFOLLOW) != 0)
		return system_failure("pool.file.identity");
	if (!S_ISREG(named.st_mode) || named.st_dev != opened.st_dev ||
	    named.st_ino != opened.st_ino || opened.st_nlink != 1)
		return -ESTALE;
	return 0;
}

int
hns5_pool_mark_hardware(struct hns5_pool *pool)
{
	static const char marker[] =
		"NEEDS_BSP_RESET: COMMON BMU VF0 PP0/LP0; stages follow\n";
	ssize_t count;
	int rc = pool_file_identity(pool);

	if (rc)
		return rc;
	count = pwrite(pool->lock_fd, marker, sizeof(marker) - 1, 0);
	if (count != (ssize_t)(sizeof(marker) - 1))
		return -EIO;
	if (fsync(pool->lock_fd) != 0)
		return system_failure("pool.marker.fsync");
	pool->touched = true;
	return 0;
}

static int
hns5_pool_open(struct hns5_pool *pool, const char *path)
{
	struct stat lock_stat;
	int rc;

	if (pool == NULL)
		return -EINVAL;
	memset(pool, 0, sizeof(*pool));
	pool->file_fd = -1;
	pool->lock_fd = -1;
	pool->dir_fd = -1;
	if (path == NULL || *path == '\0')
		return -EINVAL;
	rc = legacy_owner_check();
	if (rc)
		return rc;
	pool->lock_fd = open(HNS5_OWNER_LOCK_PATH,
			O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (pool->lock_fd < 0)
		return system_failure("pool.lock.open");
	if (flock(pool->lock_fd, LOCK_EX | LOCK_NB) != 0) {
		rc = system_failure("pool.lock.flock");
		close(pool->lock_fd);
		pool->lock_fd = -1;
		return rc;
	}
	if (fstat(pool->lock_fd, &lock_stat) != 0) {
		rc = system_failure("pool.lock.stat");
		goto fail;
	}
	if (!S_ISREG(lock_stat.st_mode) || lock_stat.st_size != 0) {
		rc = -EBUSY;
		goto fail;
	}
	pool->path = strdup(path);
	if (pool->path == NULL) {
		rc = -ENOMEM;
		goto fail;
	}
	rc = hns5_hugepage_open_directory(path);
	if (rc < 0)
		goto fail;
	pool->dir_fd = rc;
	pool->file_fd = openat(pool->dir_fd, strrchr(path, '/') + 1,
			O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (pool->file_fd < 0) {
		rc = system_failure("pool.file.create");
		goto fail;
	}
	rc = map_pool(pool);
	if (rc != 0)
		goto fail;
	return 0;
fail:
	if (pool->path != NULL) {
		free(pool->path);
		pool->path = NULL;
	}
	if (pool->file_fd >= 0) {
		close(pool->file_fd);
		pool->file_fd = -1;
	}
	if (pool->dir_fd >= 0) {
		close(pool->dir_fd);
		pool->dir_fd = -1;
	}
	if (pool->lock_fd >= 0) {
		close(pool->lock_fd);
		pool->lock_fd = -1;
	}
	return rc;
}

static int
hns5_pool_close(struct hns5_pool *pool)
{
	int rc = 0;

	if (pool->touched && !pool->recovered) {
		HNS5_LOG(ERR, "pool retained with recovery marker; BSP reset required");
		return -EIO;
	}
	if (pool->va != NULL && munmap(pool->va, HNS5_POOL_BYTES) != 0)
		rc = system_failure("pool.munmap");
	if (rc == 0 && pool->file_fd >= 0) {
		rc = pool_file_identity(pool);
		if (!rc && pool->dir_fd >= 0 && pool->path != NULL &&
		    unlinkat(pool->dir_fd, strrchr(pool->path, '/') + 1, 0) != 0)
			rc = system_failure("pool.file.unlink");
	}
	if (pool->file_fd >= 0 && close(pool->file_fd) != 0 && !rc)
		rc = system_failure("pool.file.close");
	if (pool->dir_fd >= 0 && close(pool->dir_fd) != 0 && !rc)
		rc = system_failure("pool.directory.close");
	if (rc == 0 && pool->recovered && pool->lock_fd >= 0) {
		if (ftruncate(pool->lock_fd, 0) != 0)
			rc = system_failure("pool.marker.clear");
		else if (fsync(pool->lock_fd) != 0)
			rc = system_failure("pool.marker.sync");
	}
	if (pool->lock_fd >= 0 && close(pool->lock_fd) != 0 && !rc)
		rc = system_failure("pool.lock.close");
	free(pool->path);
	pool->path = NULL;
	pool->va = NULL;
	pool->file_fd = -1;
	pool->lock_fd = -1;
	pool->dir_fd = -1;
	return rc;
}

/* ---------------------------------------------------------------- bmu reset */

int
hns5_bmu_reset(void)
{
	struct hidrv_urm_dev_info info = {
		.dev_base_name = "bmu_vf",
		.module_id = URM_BMU,
		.dev_type = HIDRV_URM_VF_DEV,
		.die_id = 0,
		.vf = { .pf_id = 0, .vf_id = 0 }
	};
	struct hidrv_urm_res registers = { 0 };
	struct ndrv_bmu_vf_open_cfg config = { 0 };
	unsigned int reset_rc;
	unsigned int close_rc;
	handle native_handle;
	int rc;

	rc = hidrv_udev_map_res(&info, "reg", &registers);
	if (rc != 0)
		return -EIO;
	if (registers.vaddr == NULL || registers.size == 0 ||
	    registers.size > UINT32_MAX) {
		hidrv_udev_unmap_res(&registers);
		return -EIO;
	}
	config.reg.io_base = registers.vaddr;
	config.reg.io_size = (unsigned int)registers.size;
	native_handle = ndrv_bmu_vf_open(&config);
	if (native_handle == NULL) {
		hidrv_udev_unmap_res(&registers);
		return -EIO;
	}
	reset_rc = ndrv_bmu_vf_reset(native_handle);
	close_rc = ndrv_bmu_vf_close(native_handle);
	hidrv_udev_unmap_res(&registers);
	return (reset_rc != 0 || close_rc != 0) ? -EIO : 0;
}

/* ---------------------------------------------------------------- runtime */

static pthread_mutex_t lifecycle = PTHREAD_MUTEX_INITIALIZER;
static struct hns5_runtime *shared;
static unsigned int users;
static _Atomic pid_t process_pid;

void
hns5_runtime_poison(struct hns5_runtime *r, const char *reason)
{
	r->poisoned = true;
	RTE_SET_USED(reason);
}

static int
start_bmu(struct hns5_runtime *r)
{
	struct hidrv_bmu_vf_cfg cfg = { .endian = HIDRV_BMU_VF_ENDIAN_LITTLE };
	struct hidrv_bmu_pp_cfg pp = {
		.phys_addr = r->pool.pa + HNS5_QUEUE_RESERVE_BYTES,
		.addr = (unsigned char *)r->pool.va + HNS5_QUEUE_RESERVE_BYTES,
		.size = HNS5_POOL_BYTES - HNS5_QUEUE_RESERVE_BYTES
	};
	struct hidrv_bmu_hard_mng_lp_cfg lp = {
		.buffer_size = HNS5_BUFFER_BYTES,
		.buffer_max = 0xffffff,
		.buffer_min = 0xffffff,
		.max_block_num = HNS5_BMU_BLOCKS,
		.min_block_num = HNS5_BMU_BLOCKS
	};
	int eth = hidrv_eth_get_devid(0, 0);

	if (eth < 0 ||
	    hidrv_eth_set_init_mode((unsigned)eth, HIDRV_INIT_HARD_MODE, 0))
		return -EIO;
	r->bmu_vf = hidrv_bmu_get_vf_devid(0, 0, 0);
	if (r->bmu_vf < 0 || hns5_pool_mark_hardware(&r->pool) ||
	    hidrv_bmu_vf_unsetup((unsigned)r->bmu_vf) || hns5_bmu_reset())
		return -EIO;
	r->bmu_vf = hidrv_bmu_get_vf_devid(0, 0, 0);
	if (r->bmu_vf < 0 || hidrv_bmu_vf_setup((unsigned)r->bmu_vf, &cfg, NULL) ||
	    hidrv_bmu_init_pp((unsigned)r->bmu_vf, 0, &pp) ||
	    hidrv_bmu_init_hard_mng_lp((unsigned)r->bmu_vf, 0, &lp) ||
	    hidrv_bmu_virt2phys((unsigned)r->bmu_vf, pp.addr) != pp.phys_addr)
		return -EIO;
	return 0;
}

int
hns5_runtime_acquire(struct hns5_runtime **out, const char *path)
{
	struct hns5_runtime *r;
	const char *configured;
	int rc;

	if (out == NULL || (process_pid && process_pid != getpid()))
		return -EINVAL;
	*out = NULL;
	if (pthread_mutex_lock(&lifecycle))
		return -EIO;
	if (shared != NULL) {
		if (shared->poisoned || users == UINT_MAX)
			rc = -EIO;
		else {
			++users;
			*out = shared;
			rc = 0;
		}
		goto done;
	}
	rc = hns5_config_pool(&configured);
	if (rc)
		goto done;
	if (path == NULL)
		path = configured;
	r = calloc(1, sizeof(*r));
	if (r == NULL) {
		rc = -ENOMEM;
		goto done;
	}
	r->owner_pid = getpid();
	r->bmu_vf = -1;
	if (pthread_mutex_init(&r->queue_lock, NULL)) {
		free(r);
		rc = -EIO;
		goto done;
	}
	rc = hns5_pool_open(&r->pool, path);
	if (rc) {
		if (r->pool.touched) {
			hns5_runtime_poison(r, "partial common initialization");
			hns5_pool_close(&r->pool);
			shared = r;
			process_pid = getpid();
		} else {
			hns5_pool_close(&r->pool);
			pthread_mutex_destroy(&r->queue_lock);
			free(r);
		}
		goto done;
	}
	rc = start_bmu(r);
	if (rc) {
		if (r->pool.touched) {
			hns5_runtime_poison(r, "partial common initialization");
			hns5_pool_close(&r->pool);
			shared = r;
			process_pid = getpid();
		} else {
			hns5_pool_close(&r->pool);
			pthread_mutex_destroy(&r->queue_lock);
			free(r);
		}
		goto done;
	}
	shared = r;
	users = 1;
	process_pid = getpid();
	*out = r;
done:
	pthread_mutex_unlock(&lifecycle);
	return rc;
}

int
hns5_runtime_release(struct hns5_runtime *r)
{
	int rc = 0;

	if (r == NULL || r->owner_pid != getpid())
		return -EINVAL;
	if (pthread_mutex_lock(&lifecycle))
		return -EIO;
	if (r != shared || !users) {
		rc = -EINVAL;
		goto done;
	}
	if (--users)
		goto done;
	pthread_mutex_lock(&r->queue_lock);
	if (r->poisoned)
		rc = -EIO;
	if (!rc && hidrv_bmu_vf_unsetup((unsigned)r->bmu_vf))
		rc = -EIO;
	if (!rc)
		rc = hns5_bmu_reset();
	if (!rc)
		r->pool.recovered = true;
	else
		hns5_runtime_poison(r, "common close/reset incomplete");
	if (hns5_pool_close(&r->pool) != 0)
		rc = -EIO;
	pthread_mutex_unlock(&r->queue_lock);
	if (!rc) {
		pthread_mutex_destroy(&r->queue_lock);
		free(r);
		shared = NULL;
	} else
		hns5_runtime_poison(r, "close incomplete");
done:
	pthread_mutex_unlock(&lifecycle);
	return rc;
}

int
hns5_runtime_lock(struct hns5_runtime *r, int *cancel_state)
{
	int rc;

	if (r == NULL || r->owner_pid != getpid())
		return -EINVAL;
	rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, cancel_state);
	if (rc)
		return -rc;
	rc = pthread_mutex_lock(&r->queue_lock);
	if (rc) {
		pthread_setcancelstate(*cancel_state, NULL);
		return -rc;
	}
	if (r->poisoned) {
		hns5_runtime_unlock(r, *cancel_state);
		return -EIO;
	}
	return 0;
}

void
hns5_runtime_unlock(struct hns5_runtime *r, int cancel_state)
{
	pthread_mutex_unlock(&r->queue_lock);
	pthread_setcancelstate(cancel_state, NULL);
}

void *
hns5_bmu_alloc(struct hns5_runtime *r, uint64_t *pa)
{
	void *p;
	uintptr_t offset;

	if (r->poisoned)
		return NULL;
	p = hidrv_bmu_pool_alloc((unsigned)r->bmu_vf, 0);
	if (p == NULL)
		return NULL;
	offset = (uintptr_t)p - (uintptr_t)r->pool.va;
	*pa = hidrv_bmu_virt2phys((unsigned)r->bmu_vf, p);
	if (offset < HNS5_QUEUE_RESERVE_BYTES ||
	    offset > HNS5_POOL_BYTES - HNS5_BUFFER_BYTES ||
	    (offset - HNS5_QUEUE_RESERVE_BYTES) % HNS5_BUFFER_BYTES ||
	    ((uintptr_t)p & 63) || (*pa & 63) ||
	    *pa != r->pool.pa + offset ||
	    hidrv_bmu_virt2phys((unsigned)r->bmu_vf,
			(unsigned char *)p + HNS5_BUFFER_BYTES - 1) !=
			    *pa + HNS5_BUFFER_BYTES - 1) {
		hns5_runtime_poison(r, "invalid BMU allocation/address");
		return NULL;
	}
	return p;
}

int
hns5_bmu_free(struct hns5_runtime *r, void *p)
{
	if (r->poisoned)
		return -EIO;
	if (hidrv_bmu_pool_free((unsigned)r->bmu_vf, p)) {
		hns5_runtime_poison(r, "BMU free failed");
		return -EIO;
	}
	return 0;
}
