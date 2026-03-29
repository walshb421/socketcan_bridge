#ifndef ASH_SERVER_CFG_H
#define ASH_SERVER_CFG_H

/* -------------------------------------------------------------------------
 * Configuration persistence  (SPEC §9)
 *
 * Saves and loads the global definition store to/from binary .ashcfg files.
 * ---------------------------------------------------------------------- */

/*
 * cfg_init: record the storage directory (NULL = persistence disabled).
 * Must be called before cfg_register_handlers() and cfg_autoload().
 */
void cfg_init(const char *storage_dir);

/* Register CFG_SAVE / CFG_LOAD message handlers with the dispatch table. */
void cfg_register_handlers(void);

/*
 * cfg_autoload: load every *.ashcfg file found in the storage directory
 * into the global definition store.  Called once at server startup.
 */
void cfg_autoload(void);

#endif /* ASH_SERVER_CFG_H */
