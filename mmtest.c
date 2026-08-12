/*
 * mmtest - exercise the HAMMER2/Linux page-cache mmap path (v0.39+).
 *
 * Before v0.39 the port had no address_space_operations and no
 * file_operations->mmap, so mmap(2) on a HAMMER2 file returned ENODEV.
 * This checks that mmap now works and, crucially, that mmap / read / write
 * present a coherent view of the same file.
 *
 *   argv[1] = directory on a mounted HAMMER2 filesystem
 *
 * Exit status 0 == all checks passed.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static int fails;

#define CHECK(cond, msg) do {						\
	if (cond) {							\
		printf("  PASS  %s\n", msg);				\
	} else {							\
		printf("  FAIL  %s (errno=%d %s)\n", msg, errno,	\
		    strerror(errno));					\
		fails++;						\
	}								\
} while (0)

int
main(int argc, char **argv)
{
	char path[4096], buf[8192], rbuf[8192];
	const char *dir = argc > 1 ? argv[1] : ".";
	size_t len = 200000;		/* > 1 folio and > 3 pages */
	int fd, i;
	char *map;

	snprintf(path, sizeof(path), "%s/mmtest.dat", dir);

	/* --- populate a file via write(2) --------------------------------- */
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { perror("open"); return 2; }
	for (i = 0; i < (int)sizeof(buf); i++)
		buf[i] = (char)(i * 7 + 3);
	{
		size_t off = 0;
		while (off < len) {
			size_t n = len - off < sizeof(buf) ? len - off : sizeof(buf);
			for (i = 0; i < (int)n; i++)
				buf[i] = (char)((off + i) * 7 + 3);
			if (write(fd, buf, n) != (ssize_t)n) { perror("write"); return 2; }
			off += n;
		}
	}
	fsync(fd);

	printf("[1] read-only mmap vs write(2) coherency\n");
	map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(map != MAP_FAILED, "mmap(PROT_READ, MAP_SHARED) succeeds (was ENODEV before v0.39)");
	if (map != MAP_FAILED) {
		int ok = 1;
		for (i = 0; i < (int)len; i++)
			if (map[i] != (char)(i * 7 + 3)) { ok = 0; break; }
		CHECK(ok, "mmap contents match what write(2) stored");
		munmap(map, len);
	}

	printf("[2] MAP_SHARED write via mmap, read back via pread(2)\n");
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(map != MAP_FAILED, "mmap(PROT_WRITE, MAP_SHARED) succeeds");
	if (map != MAP_FAILED) {
		/* overwrite a span that crosses a page boundary */
		for (i = 0; i < 9000; i++)
			map[5000 + i] = (char)(0xA5 ^ i);
		CHECK(msync(map, len, MS_SYNC) == 0, "msync(MS_SYNC) succeeds");
		munmap(map, len);

		if (pread(fd, rbuf, sizeof(rbuf), 4000) == sizeof(rbuf)) {
			int ok = 1;
			for (i = 0; i < (int)sizeof(rbuf); i++) {
				off_t o = 4000 + i;
				char want = (o >= 5000 && o < 14000)
				    ? (char)(0xA5 ^ (o - 5000))
				    : (char)(o * 7 + 3);
				if (rbuf[i] != want) { ok = 0; break; }
			}
			CHECK(ok, "pread(2) sees the mmap-written bytes (page-cache coherent)");
		} else {
			CHECK(0, "pread after mmap write");
		}
	}

	printf("[3] MAP_PRIVATE copy-on-write isolation\n");
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	CHECK(map != MAP_FAILED, "mmap(MAP_PRIVATE) succeeds");
	if (map != MAP_FAILED) {
		map[100] = 0x11;		/* private dirty */
		CHECK(map[100] == 0x11, "private write visible in the mapping");
		munmap(map, len);
		if (pread(fd, rbuf, 1, 100) == 1)
			CHECK(rbuf[0] != 0x11, "private write did NOT leak to the file");
	}
	close(fd);

	printf("[4] persistence: reopen and verify the msync'd data\n");
	fd = open(path, O_RDONLY);
	CHECK(fd >= 0, "reopen file");
	if (fd >= 0) {
		if (pread(fd, rbuf, sizeof(rbuf), 4000) == sizeof(rbuf)) {
			int ok = 1;
			for (i = 0; i < (int)sizeof(rbuf); i++) {
				off_t o = 4000 + i;
				char want = (o >= 5000 && o < 14000)
				    ? (char)(0xA5 ^ (o - 5000))
				    : (char)(o * 7 + 3);
				if (rbuf[i] != want) { ok = 0; break; }
			}
			CHECK(ok, "msync'd mmap writes survived close/reopen");
		}
		close(fd);
	}

	printf("\n%s (%d failure%s)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED",
	    fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
