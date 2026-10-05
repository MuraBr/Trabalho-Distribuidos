#ifndef CHORD_H
#define CHORD_H

#include "node.h"
#include <stddef.h>

#define CHORD_FINGER_COUNT 256U

typedef struct { NodeID id; NodeConfig config; } ChordPeer;
typedef struct Chord Chord;

int chord_create(const Node *local, Chord **output);
void chord_destroy(Chord *chord);
void chord_id_add_power(const NodeID *base, unsigned bit, NodeID *result);
int chord_interval_open_closed(const NodeID *start, const NodeID *end, const NodeID *value);
int chord_interval_open_open(const NodeID *start, const NodeID *end, const NodeID *value);
int chord_route(Chord *chord, const NodeID *key, ChordPeer *next, int *complete);
int chord_get_successor(Chord *chord, ChordPeer *output);
int chord_get_predecessor(Chord *chord, ChordPeer *output, int *exists);
int chord_get_finger(Chord *chord, unsigned index, ChordPeer *output);
int chord_set_successor(Chord *chord, const ChordPeer *peer);
int chord_notify(Chord *chord, const ChordPeer *peer);
int chord_consider_predecessor(Chord *chord, const ChordPeer *candidate);
int chord_set_finger(Chord *chord, unsigned index, const ChordPeer *peer);
int chord_local(Chord *chord, ChordPeer *output);

#endif
