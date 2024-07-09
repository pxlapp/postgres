/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/bin/psql/copilot.h
 */
#ifndef PXL_H
#define PXL_H


bool pxl_init(void);
void pxl_tickle(void);
void pxl_cancel_inflight_requests(void);
void pxl_chat(char *query);

#endif							/* PXL_H */
