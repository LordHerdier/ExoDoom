/*
 * doom_net_stub.c — the two Doom globals that belong to files we do not
 * vendor (SCRUM-65).
 *
 * `nm -u` over the compiled src/doom/ objects lists 52 external symbols.
 * Fifty of them are libc or the DG_* platform callbacks.  The other two are
 * these: Doom's own `drone` and `net_client_connected`, declared in the
 * vendored src/doom/net_client.h and defined in net_client.c -- one of the
 * networking files SCRUM-63 deliberately left out of the vendor, along with
 * net_server.c, net_io.c and the rest of the multiplayer stack.
 *
 * So they are not a libc gap and not a stub in the sense of "a function we
 * have not written yet".  They are two booleans describing a subsystem this
 * port does not have, and the honest values are the ones a single-player
 * game running with no network stack actually has:
 *
 *   net_client_connected  false -- there is no client, no server, no socket.
 *   drone                 false -- drone mode is a spectator client that
 *                                  connects to a server and generates no
 *                                  ticcmds; without networking it cannot be
 *                                  entered.  src/doom/d_loop.c:530 sets
 *                                  connect_data->drone from a "-drone"
 *                                  command line parameter, but that value
 *                                  travels through the networking code that
 *                                  is not here, so nothing can ever set this
 *                                  one.
 *
 * Both are read in src/doom/d_loop.c (11 sites between them) and both gate
 * code paths that would call into the absent networking: with them false,
 * TryRunTics takes its single-player branch and D_ArbitrateNetStart is never
 * reached.  Writing them as `false` is therefore not papering over missing
 * functionality -- it is stating the configuration, and it is what keeps
 * those call sites from being compiled against a network layer that would
 * fail at link.
 *
 * ── Why here and not in src/doom/ ──────────────────────────────────────
 *
 * Adding a hand-written net_client.c to the vendored tree would make the
 * next re-vendor (SCRUM-63's whole point is that src/doom/ stays a clean
 * drop-in) collide with a file upstream also ships.  This file sits outside
 * it, gets picked up by build.sh's ordinary C-source glob, and includes
 * doomtype.h through the same "doom/..." quoted path src/doomgeneric_exo.h
 * already uses -- so `boolean` is Doom's own typedef here rather than an
 * `int` that merely happens to link.
 *
 * If real networking ever arrives, deleting this file is the whole
 * migration: net_client.c would define both.
 */

#include "doom/doomtype.h"

boolean net_client_connected = false;
boolean drone               = false;
