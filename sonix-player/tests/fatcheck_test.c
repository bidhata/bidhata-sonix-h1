// Runs the card checker on an image and prints what it found on one line,
// for tests/fatcheck_test.sh to compare:
//
//     fatcheck_test IMAGE check
//     fatcheck_test IMAGE repair [FILES FOLDERS]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system/device/fatcheck.h"

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s IMAGE check|repair [FILES FOLDERS]\n", argv[0]);
		return 2;
	}
	bool repair = strcmp(argv[2], "repair") == 0;
	long files = argc > 4 ? atol(argv[3]) : -1;
	long folders = argc > 4 ? atol(argv[4]) : -1;

	static const char *const names[] = {"clean", "lost", "damaged", "unsupported", "mismatch", "io", "no-memory"};
	fatcheck_result_t r;
	bool ran = fatcheck_run(argv[1], repair, files, folders, &r);
	printf("%s files=%u folders=%u lost=%u bytes=%llu repaired=%d ran=%d%s%s\n", names[r.status], (unsigned)r.files, (unsigned)r.folders, (unsigned)r.lost_clusters, (unsigned long long)r.lost_bytes, r.repaired, ran, r.problem[0] ? " -- " : "", r.problem);
	return 0;
}
