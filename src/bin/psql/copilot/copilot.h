/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/bin/psql/copilot.h
 */
#ifndef COPILOT_H
#define COPILOT_H

void copilot_init(void);
void copilot_refresh_schema(void);
int copilot_get_request(
    char **session_id,
    char **prompt_buffer,
    bool *is_prime
);

void copilot_on_completion(char *token, bool stop, bool error, bool cancel);
void copilot_on_chat(char *token, bool stop, bool error, bool cancel);

void copilot_chat(char *query);
void copilot_explain(char *query);

#endif							/* COPILOT_H */
