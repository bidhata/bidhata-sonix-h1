#ifndef CARDTEST_H
#define CARDTEST_H

#include <stdbool.h>

// A short write-and-read test of the card, for the card page: how fast it
// takes a file and gives it back, and whether what comes back is what went in.
//
// It writes CARDTEST_BYTES to a file of its own in the player's folder on the
// card, forces it out to the card, reads it back from the card and not from
// memory, compares, and deletes the file. That says how the card behaves over
// those megabytes, here and now. It does not say the card is the size it
// claims: a card that lies about its capacity fails further in than this goes.

#define CARDTEST_BYTES (16u * 1024u * 1024u)

typedef enum {
	CARDTEST_OK,
	CARDTEST_NO_CARD,  // no card, with a computer, or read-only
	CARDTEST_NO_ROOM,  // not twice the test's size free
	CARDTEST_IO_ERROR, // a write or a read failed
	CARDTEST_MISMATCH, // read back, and not what was written
} cardtest_status_t;

typedef struct {
	cardtest_status_t status;
	int write_kb_s; // kilobytes (1000 bytes) a second
	int read_kb_s;
} cardtest_result_t;

// Blocks for the length of the test, a few seconds: never on the UI thread.
void cardtest_run(cardtest_result_t *out);

#endif /* CARDTEST_H */
