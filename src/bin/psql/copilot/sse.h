#ifndef SSE_H
#define SSE_H

#include "postgres_fe.h"
#include "lib/stringinfo.h"

typedef enum {
    SSE_PARSE_ERROR,
    SSE_PARSE_CONTINUE,
    SSE_PARSE_FINISHED,
} sse_parse_result;

typedef enum {
    SSE_PARSE_STATE_INIT,
    SSE_PARSE_STATE_PREAMBLE_D,
    SSE_PARSE_STATE_PREAMBLE_A1,
    SSE_PARSE_STATE_PREAMBLE_T,
    SSE_PARSE_STATE_PREAMBLE_A2,
    SSE_PARSE_STATE_PREAMBLE_COLON,
    SSE_PARSE_STATE_WS,
    SSE_PARSE_STATE_PAYLOAD,
} sse_parse_state;

typedef void (*sse_callback)(char *token, bool stop, bool error, bool cancel);
typedef struct {
    int id;
    sse_parse_state state;
    StringInfo buffer;
    sse_callback cb;
} sse;

sse *sse_init(int id, sse_callback cb);
void sse_clean(sse *s);
int sse_parse(sse *s, const char *ptr, size_t len);

#endif
