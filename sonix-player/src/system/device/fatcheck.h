#ifndef FATCHECK_H
#define FATCHECK_H

#include <stdbool.h>
#include <stdint.h>

// Checks a FAT32 or an exFAT card that is NOT mounted, and can give back the
// space of clusters no file owns.
//
// What it does is small on purpose. It reads the whole tree, follows every
// file's and folder's clusters, and then looks for clusters the card calls
// taken that nothing in the tree reached: "lost" clusters, what a card is
// left with when it is pulled out or the power goes while a file is being
// written. Those it can free, and that is the only thing it ever writes: on
// FAT32, zeroes in the allocation table for clusters it has just seen to be
// nobody's, and the count of free clusters beside it; on exFAT, their bits in
// the allocation bitmap.
//
// Anything else it finds wrong -- two files sharing a cluster, a chain that
// ends in free space, the two copies of a FAT32 table differing, an exFAT
// file whose clusters the bitmap calls free -- it reports and leaves alone.
// Putting those right means deciding which file is the true one, and that is
// a computer's job (chkdsk, fsck.fat, fsck.exfat). And with anything of that
// kind present it frees nothing either: a tree that cannot be read in full
// cannot say which clusters are really nobody's.
//
// It needs no memory to speak of: one bit per cluster, a buffer of a cluster
// or less, and on exFAT the bitmap, which is another bit per cluster; the
// allocation table is read through the page cache. That is what lets it run
// on the player at all, where the usual tools do not fit.

typedef enum {
	FATCHECK_CLEAN,		  // nothing wrong
	FATCHECK_LOST,		  // lost clusters and nothing else; `repaired` says whether they were freed
	FATCHECK_DAMAGED,	  // something this does not repair; `problem` says what
	FATCHECK_UNSUPPORTED, // neither FAT32 nor exFAT, or a variant of them this does not read
	FATCHECK_MISMATCH,	  // the tree read here is not the tree the caller counted: nothing was freed
	FATCHECK_IO,		  // the card could not be read, or a write failed
	FATCHECK_NO_MEMORY,
} fatcheck_status_t;

typedef struct {
	fatcheck_status_t status;
	uint32_t files, folders;  // what the tree holds, the root folder not counted
	uint32_t lost_clusters;	  // as found, whether or not they were then freed
	uint64_t lost_bytes;
	bool repaired;
	// For the log: what was found, in a few words of English. Never shown.
	char problem[96];
} fatcheck_result_t;

// Checks `device` (a partition, or an image of one). With `repair`, lost
// clusters are freed when they are all that is wrong.
//
// `expect_files` and `expect_folders` are what the caller counted in the same
// tree through the mounted filesystem just before unmounting it, or -1 for
// "not counted". With `repair`, nothing is freed unless both agree with what
// was read here: the kernel's reading of the folders and this one's are two
// separate pieces of code, and a cluster is only called lost when both saw
// the same tree.
//
// Returns false when the check could not be carried through (status says
// why), true when `out` describes the card.
bool fatcheck_run(const char *device, bool repair, long expect_files, long expect_folders, fatcheck_result_t *out);

#endif /* FATCHECK_H */
