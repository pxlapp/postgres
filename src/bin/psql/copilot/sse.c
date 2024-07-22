#include "sse.h"

#include "common/jsonapi.h"
#include "mb/pg_wchar.h"


static void parse(sse *s);

sse *
sse_init(int id, sse_callback cb) {
    sse *r = malloc(sizeof(sse));
    r->state = SSE_PARSE_STATE_PREAMBLE_D;
    r->id = id;
    r->buffer = makeStringInfo();
    r->cb = cb;
    return r;
}

void
sse_clean(sse *s) {
    resetStringInfo(s->buffer);
    free(s);
}

int
sse_parse(sse *s, const char *str, size_t len) {
    /*FILE *fd = fopen("/tmp/sse.log", "a");*/
    /*fprintf(fd, "<payload>%lu:%s</tpayload>\n", len, str);*/
    /*fclose(fd);*/
    const char *tmp = str;
    while (len > 0) {
        switch (s->state) {
            case SSE_PARSE_STATE_PREAMBLE_D:
                if (*tmp != 'd') return SSE_PARSE_ERROR;
                s->state++;
                break;
            case SSE_PARSE_STATE_PREAMBLE_A1:
                if (*tmp != 'a') return SSE_PARSE_ERROR;
                s->state++;
                break;
            case SSE_PARSE_STATE_PREAMBLE_T:
                if (*tmp != 't') return SSE_PARSE_ERROR;
                s->state++;
                break;
            case SSE_PARSE_STATE_PREAMBLE_A2:
                if (*tmp != 'a') return SSE_PARSE_ERROR;
                s->state++;
                break;
            case SSE_PARSE_STATE_PREAMBLE_COLON:
                if (*tmp != ':') return SSE_PARSE_ERROR;
                s->state++;
                break;
            case SSE_PARSE_STATE_WS:
                if (*tmp == ' ') {
                    break;
                }
                s->state++;
                // we want to fall into next state here
                // hence no break
            case SSE_PARSE_STATE_PAYLOAD:
                if (*tmp != '\r' && *tmp != '\n') {
                    appendStringInfoChar(s->buffer, *tmp);
                    break;
                }
                parse(s);
                resetStringInfo(s->buffer);
                s->state = SSE_PARSE_STATE_INIT;
                // we want to fall into next state here
                // hence no break
            case SSE_PARSE_STATE_INIT:
                if (*tmp != '\r' && *tmp != '\n') {
                    s->state = SSE_PARSE_STATE_PREAMBLE_D;
                    continue;
                }
                break;
        }
        tmp++;
        len--;
    }
    return SSE_PARSE_CONTINUE;
}


typedef enum {
    SSE_DATA_PARSE_STATE_INIT,
    SSE_DATA_PARSE_STATE_TOKEN,
    SSE_DATA_PARSE_STATE_STOP,
    SSE_DATA_PARSE_STATE_ERROR,
    SSE_DATA_PARSE_STATE_CANCEL
} sse_data_parse_state;

typedef struct {
    sse_data_parse_state s;
    char *token;
    bool stop;
    bool error;
    bool cancel;
} sse_elem_parse_state;

static JsonParseErrorType
sse_json_key(void *state, char *fname, bool isnull) {
    sse_elem_parse_state *s = state;
    if (strcmp(fname, "token") == 0) {
        s->s = SSE_DATA_PARSE_STATE_TOKEN;
    } else if (strcmp(fname, "stop") == 0) {
        s->s = SSE_DATA_PARSE_STATE_STOP;
    } else if (strcmp(fname, "error") == 0) {
        s->s = SSE_DATA_PARSE_STATE_ERROR;
    } else if (strcmp(fname, "cancel") == 0) {
        s->s = SSE_DATA_PARSE_STATE_CANCEL;
    }
    return JSON_SUCCESS;
}


static JsonParseErrorType
sse_json_value(void *state, char *token, JsonTokenType token_type) {
    sse_elem_parse_state *s = state;
    if (s->s == SSE_DATA_PARSE_STATE_TOKEN && token_type == JSON_TOKEN_STRING) {
        s->token = token;
    } else if (s->s == SSE_DATA_PARSE_STATE_STOP && token_type == JSON_TOKEN_TRUE) {
        s->stop = true;
    } else if (s->s == SSE_DATA_PARSE_STATE_ERROR && token_type == JSON_TOKEN_TRUE) {
        s->error = true;
    } else if (s->s == SSE_DATA_PARSE_STATE_CANCEL && token_type == JSON_TOKEN_TRUE) {
        s->cancel = true;
    }
    return JSON_SUCCESS;
}


static void
parse(sse *s) {
    JsonLexContext *lex;
    JsonSemAction sem;
    JsonParseErrorType json_error;
    sse_elem_parse_state p;

    lex = makeJsonLexContextCstringLen(
        NULL,
        s->buffer->data,
        s->buffer->len,
        PG_UTF8,
        true
    );
    memset(&sem, 0, sizeof(sem));
    memset(&p, 0, sizeof(p));
    sem.semstate = &p;
    sem.object_field_start = sse_json_key;
    sem.scalar = sse_json_value;

    json_error = pg_parse_json(lex, &sem);
    if (json_error == JSON_SUCCESS) {
        s->cb(p.token, p.stop, p.error, p.cancel);
    }
    freeJsonLexContext(lex);
}
