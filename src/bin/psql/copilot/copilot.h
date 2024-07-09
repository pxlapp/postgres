/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/bin/psql/copilot.h
 */
#ifndef COPILOT_H
#define COPILOT_H

#ifdef HAVE_LIBCURL
#define COPILOT_ENABLED 1
#endif

void copilot_init(void);
void copilot_refresh_schema(void);
int copilot_get_meta(
    char **session_id,
    char **schema_buffer
);
int copilot_get_request(
    char **session_id,
    char **schema_buffer,
    char **prompt_buffer,
    bool *is_prime
);

void copilot_on_completion(char *token, bool stop, bool error, bool cancel);
void copilot_on_chat(char *token, bool stop, bool error, bool cancel);

#endif							/* COPILOT_H */
