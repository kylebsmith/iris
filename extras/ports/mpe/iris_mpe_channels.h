/* ============================================================================
   iris_mpe_channels.h  —  which member channel does this note go on?
   C99 · no dependencies · no malloc · header-only · platform-free

   MPE v1.1 §2.2.4.1 is a "shall": every new note gets its own unoccupied
   Member Channel until there are none, and then

       "When there are more notes than unoccupied Channels, a new note shall
        share a MIDI Channel with an existing note."

   Sharing is the specified degradation. Dropping the note is not, and
   stealing an older one is not. v1.0 §1.2 says why: sharing "can be
   preferable to limiting polyphony by preventing a new note from sounding,
   or stopping an older note."

   But sharing is not free, and this is the whole reason this file exists as
   something separate and testable: two notes on one channel are ONE
   expressive voice. Pitch Bend, Channel Pressure and CC 74 are CHANNEL
   messages, so from the moment two notes share a channel they bend together,
   swell together and slide together. The note still sounds. It has stopped
   being independently expressive, which is the entire point of MPE.

   So iris_mpe_pool_take() reports sharing back to the caller, and the pool
   keeps a running count. Degrading is required; degrading quietly is not.

   THE ORDER OF PREFERENCE is MPE v1.1 Appendix A.3, in its own words:

     - "Simple circular assignment of new notes to Member Channels of a Zone
        will not usually provide satisfactory results."
     - "In the simplest workable implementation, a new note will be assigned
        to the Channel with the lowest count of Active Notes."
     - "Then, all else being equal, the Channel with the oldest last Note Off
        would be preferred."
     - "MPE controllers can preferentially re-use a Channel that has been
        most recently deployed to play a certain Note Number once the previous
        note has entered its Note Off state. This avoids stacking and
        chorusing identical notes."

   Which collapses to: lowest active count, and among equals, the channel that
   last played THIS note number, else the one released longest ago. JUCE's
   MPEChannelAssigner and LinnStrument's ChannelBucket both land here by
   different routes; LinnStrument's comment is the clearest statement of the
   goal — "postponing the reuse of a channel as much as possible is important
   for sounds that have long releases."

   Fixed array, no malloc, no linked list, sized at compile time.
   ============================================================================ */

#ifndef IRIS_MPE_CHANNELS_H
#define IRIS_MPE_CHANNELS_H

#include <stdint.h>

#define IRIS_MPE_MAX_MEMBERS 15   /* a zone may claim the other zone's manager */

typedef struct {
  uint8_t  active;      /* notes sounding on this channel right now */
  int16_t  last_note;   /* the note number it last carried, or -1 */
  uint32_t freed;       /* sequence number of its most recent Note Off */
} iris_mpe_chan;

typedef struct {
  iris_mpe_chan c[IRIS_MPE_MAX_MEMBERS];
  uint8_t     n;        /* member channels in the zone */
  uint32_t    seq;      /* monotonic; only ever compared, never displayed */
  uint32_t    shared;   /* notes that had to share. NOT allowed to be ignored */
} iris_mpe_pool;

static inline void iris_mpe_pool_init(iris_mpe_pool *p, int n) {
  p->n = (uint8_t)n; p->seq = 0; p->shared = 0;
  for (int i = 0; i < IRIS_MPE_MAX_MEMBERS; ++i) {
    p->c[i].active = 0; p->c[i].last_note = -1; p->c[i].freed = 0;
  }
}

/* Choose a channel for `note`. Never fails, because the spec does not permit
   it to fail. *shared is set to 1 when the returned channel is already
   carrying a note, in which case the caller MUST surface that. */
static inline int iris_mpe_pool_take(iris_mpe_pool *p, int note, int *shared) {
  int best = 0, i;
  uint8_t min_active = 255;

  *shared = 0;

  /* A.3, last bullet: a free channel that last played this very note number
     is the best possible home for it — it cannot stack or chorus against
     itself, and any release tail still ringing there is the same pitch. */
  for (i = 0; i < p->n; ++i)
    if (p->c[i].active == 0 && p->c[i].last_note == note) { best = i; goto taken; }

  /* Otherwise: lowest active count, ties broken by oldest last Note Off. */
  for (i = 0; i < p->n; ++i) {
    if (p->c[i].active < min_active ||
        (p->c[i].active == min_active && p->c[i].freed < p->c[best].freed)) {
      best = i; min_active = p->c[i].active;
    }
  }
  if (p->c[best].active > 0) { *shared = 1; p->shared++; }

taken:
  p->c[best].active++;
  p->c[best].last_note = (int16_t)note;
  return best;
}

static inline void iris_mpe_pool_give(iris_mpe_pool *p, int ch, int note) {
  if (ch < 0 || ch >= p->n) return;
  if (p->c[ch].active) p->c[ch].active--;
  p->c[ch].last_note = (int16_t)note;
  p->c[ch].freed = ++p->seq;
}

#endif /* IRIS_MPE_CHANNELS_H */
