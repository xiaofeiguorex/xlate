#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/aes.h>

#include <fcntl.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <libelf.h>
#include <gelf.h>

#include <xlate/elf.h>
#include <xlate/macros.h>

#include <xlate/x86-64/clflush.h>
#include <xlate/x86-64/time.h>

void *crypto_find_te0(int fd)
{
	Elf *elf;
	void *base;

	elf_version(EV_CURRENT);

	elf = elf_begin(fd, ELF_C_READ, NULL);

	if (!(base = gelf_find_sym_ptr(elf, "Te0"))) {
		fprintf(stderr, "error: unable to find the 'Te0' symbol in "
			"libcrypto.so.\n\nPlease compile a version of OpenSSL with the "
			"T-table AES implementation enabled.\n");
		return NULL;
	}

	elf_end(elf);

	return base;
}

unsigned char key_data[32] = { 0 };

unsigned char plain[16];
unsigned char cipher[128];
unsigned char restored[128];

int cmp_u64(const void *lhs, const void *rhs)
{
	return memcmp(lhs, rhs, sizeof(uint64_t));
}

int main(int argc, char *argv[])
{
	uint64_t *timings;
	AES_KEY key;
	uint64_t time, dt, t0;
	struct stat stat;
	int fd;
	char *base;
	char *te0;
	char *cl;
	size_t size;
	size_t round;
	size_t i, j;
	size_t byte;

	timings = malloc(1000000 * sizeof *timings);

	if (argc < 2) {
		fprintf(stderr, "%s <path>\n", argv[0]);
		return -1;
	}

	if ((fd = open(argv[1], O_RDONLY)) < 0) {
		perror("open");
		return -1;
	}

	if (!(te0 = crypto_find_te0(fd)))
		return -1;

	if (fstat(fd, &stat) < 0) {
		fprintf(stderr, "error: unable to get the file size of libcrypto.so\n");
		return -1;
	}

	size = ALIGN_UP(stat.st_size, 4 * KIB);

	if ((base = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		fprintf(stderr, "error: unable to map libcrypto.so\n");
		return -1;
	}

	AES_set_encrypt_key(key_data, 128, &key);


	for (cl = base + (size_t)te0, j = 0; j < 16; ++j, cl += 64) {
		struct timespec past, now;
		double diff;

		clock_gettime(CLOCK_MONOTONIC, &past);

		for (byte = 0; byte < 256; byte += 16) {
			plain[0] = byte;

			AES_encrypt(plain, cipher, &key);

			time = UINT64_MAX;

			for (round = 0; round < 1000000; ++round) {
				sched_yield();

				for (i = 1; i < 16; ++i) {
					plain[i] = rand() % 256;
				}

				AES_encrypt(plain, cipher, &key);
				sched_yield();

				asm volatile("mfence\n" ::: "memory");
				t0 = rdtsc();
				asm volatile("mfence\n" ::: "memory");
				clflush(cl);
				asm volatile("mfence\n" ::: "memory");
				dt = rdtsc() - t0;
				asm volatile("mfence\n" ::: "memory");

				if (time > dt)
					time = dt;

				timings[round] = dt;
			}

			qsort(timings, round, sizeof *timings, cmp_u64);

			printf("%" PRIu64 " ", time + timings[round / 2]);
			fflush(stdout);
		}

		clock_gettime(CLOCK_MONOTONIC, &now);

		diff = now.tv_sec + now.tv_nsec * .000000001;
		diff -= past.tv_sec + past.tv_nsec * .000000001;
		fprintf(stderr, "time: %.03lfs\n", diff);

		printf("\n");
	}

	close(fd);

	return 0;
}
