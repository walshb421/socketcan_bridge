#ifndef ASH_SERVER_DEF_H
#define ASH_SERVER_DEF_H

/* -------------------------------------------------------------------------
 * Signal/PDU/Frame definition store  (SPEC §6, §8.5)
 *
 * Global, session-agnostic in-memory namespace.  Definitions survive client
 * disconnects.  All three definition types share a single name namespace:
 * a name may not be used simultaneously for different types.
 * ---------------------------------------------------------------------- */

void def_init(void);
void def_destroy(void);
void def_register_handlers(void);

#endif /* ASH_SERVER_DEF_H */
