// SPDX-License-Identifier: MIT
// Copyright (c) 2014 Richard Grenville <pyxlcy@gmail.com>

#include "compiler.h"
#include "log.h"

#include "xrescheck.h"

static xrc_xid_record_t *gs_xid_records = NULL;

#define HASH_ADD_XID(head, xidfield, add) HASH_ADD(hh, head, xidfield, sizeof(xid), add)

#define HASH_FIND_XID(head, findxid, out) HASH_FIND(hh, head, findxid, sizeof(xid), out)

#define M_CPY_POS_DATA(prec)                                                             \
	prec->file = file;                                                               \
	prec->func = func;                                                               \
	prec->line = line;

/**
 * @brief Add a record of given XID to the allocation table.
 */
void xrc_add_xid_(XID xid, const char *type, M_POS_DATA_PARAMS) {
	auto prec = ccalloc(1, xrc_xid_record_t);
	prec->xid = xid;
	prec->type = type;
	M_CPY_POS_DATA(prec);

	HASH_ADD_XID(gs_xid_records, xid, prec);
}

/**
 * @brief Delete a record of given XID in the allocation table.
 */
void xrc_delete_xid_(XID xid, M_POS_DATA_PARAMS) {
	xrc_xid_record_t *prec = NULL;
	HASH_FIND_XID(gs_xid_records, &xid, prec);
	if (!prec) {
		log_error("XRC: %s:%d %s(): Can't find XID %#010lx we want to delete.",
		          file, line, func, xid);
		return;
	}
	HASH_DEL(gs_xid_records, prec);
	free(prec);
}

/**
 * @brief Report about issues found in the XID allocation table.
 */
void xrc_report_xid(void) {
	for (xrc_xid_record_t *prec = gs_xid_records; prec; prec = prec->hh.next)
		log_trace("XRC: %s:%d %s(): %#010lx (%s) not freed.\n", prec->file,
		          prec->line, prec->func, prec->xid, prec->type);
}

/**
 * @brief Clear the XID allocation table.
 */
void xrc_clear_xid(void) {
	xrc_xid_record_t *prec = NULL, *ptmp = NULL;
	HASH_ITER(hh, gs_xid_records, prec, ptmp) {
		HASH_DEL(gs_xid_records, prec);
		free(prec);
	}
}
