#include <stdio.h>
#include <pthread.h>
#include <termios.h>
#include <unistd.h>
#include <readline/readline.h>

#include "postgres_fe.h"

#include "common.h"
#include "common/jsonapi.h"
#include "common/logging.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "port.h"
#include "settings.h"
#include "copilot.h"
#include "pxl.h"
#include "sse.h"
#include "tab-complete.h"


/*#define COPILOT_DEBUG 1*/
#define SESSION_ID_SIZE 16
#define COPILOT_BUFFER_SIZE 40*1024


static StringInfo schema;
static StringInfo prompt;

static char sid[2*SESSION_ID_SIZE + 1];
static bool session_primed;
static bool prime;

static pthread_mutex_t mtx;
static char input_buffer[COPILOT_BUFFER_SIZE];
static char completion_buffer[COPILOT_BUFFER_SIZE];
static int num_completion_lines = 0;
static int completion_buffer_idx, last_completion_buffer_idx;

static void escape_json(StringInfo buf, const char *str);

static void
copilot_redisplay() {
#ifndef COPILOT_ENABLED
    return rl_redisplay();
#else
    char tmp_buffer[32];

    /* clear out the previous completion lines
     * we do this by moving down num_completion_lines
     * and then clearing the line + moving cursor up one line
     * all the way until current input line
     * when we reach the current input line we also do a
     * carriage return to force the cursor back to col 1
     */
    if (num_completion_lines) {
        sprintf(tmp_buffer, "\033[%dB", num_completion_lines);
        write(STDOUT_FILENO, tmp_buffer, strlen(tmp_buffer));
    }

    while (num_completion_lines-- > 0) {
        write(STDOUT_FILENO, "\033[2K\033[A", 7);
    }

    /* reset current line */
    write(STDOUT_FILENO, "\033[2K\r", 5);
    num_completion_lines = 0;

    /* tell readline to reset itself for sol settings */
    rl_on_new_line();

    /* do the normal readline display
     * at this point we've kind of hack-forced readline
     * to do a re-print of the current input line
     */
    rl_redisplay();

    if (completion_buffer_idx != 0) {
        /* ansi codes to:
         *  - save cursor
         *  - move cursor to column 9999
         *  - read cursor position to obtain screen width
         *  - restore cursor position
         *  - read cursor position to obtain current loc
         *  - set text color to gray
         */
        char prefix[] = "\033[s\033[;9999H\033[6n\033[u\033[6n\033[90m";
        /* ansi codes to:
         *  - set text color to white
         *  - read cursor position to obtain end row
         */
        char postfix[] = "\033[0m\033[6n";
        int num_chars_current_line;
        int x1, xs, y2, y3;
        struct termios term_new, term_orig;

        /* ensure rl_redisplay is flushed out to device
         * before we start our hacking
         */
        fflush(stdout);

        /* turn off (canonical & echo) terminal settings */
        tcgetattr(STDOUT_FILENO, &term_new);
        tcgetattr(STDOUT_FILENO, &term_orig);
        term_new.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDOUT_FILENO, TCSANOW, &term_new);

        /* write prefix + completion_buffer + postfix */
        write(STDOUT_FILENO, prefix, strlen(prefix));
        write(STDOUT_FILENO, completion_buffer, completion_buffer_idx);
        write(STDOUT_FILENO, postfix, strlen(postfix));

        /* read the three cursor positions we've queries in prefix & postfix
         * first is to read screen width
         * second is to read cursor column prior to printing completion buffer
         * third is to read cursor row after printing completion buffer
         */
        for (int i = 0; i < 3; i++) {
            int state = 0, x = 0, y = 0;
            while (true) {
                char c;
                int n = read(STDIN_FILENO, &c, sizeof(c));
                if (n != 1) {
                    return;
                }
                switch (state) {
                    case 0:
                        if (c == '\033') {
                            state++;
                            break;
                        }
                        return;
                    case 1:
                        if (c == '[') {
                            state++;
                            break;
                        }
                        return;
                    case 2:
                        if (c == ';') {
                            state++;
                            break;
                        } else if (c >= '0' && c <= '9') {
                            y = y * 10 + (c - '0');
                            break;
                        }
                        return;
                    case 3:
                        if (c == 'R') {
                            state++;
                            break;
                        } else if (c >= '0' && c <= '9') {
                            x = x * 10 + (c - '0');
                            break;
                        }
                }
                if (state == 4) {
                    break;
                }
            }
            switch (i) {
                case 0:
                    xs = x;
                    break;
                case 1:
                    x1 = x;
                    break;
                case 2:
                    y2 = y;
                    break;
            }
        }

        /* restore terminal settings */
        tcsetattr(STDOUT_FILENO, TCSANOW, &term_orig);

        /* calculate how many lines we printed as a result
         * of printing completion buffer. we try to take into
         * account wrapped lines
         * we also clip the number of lines to max screen
         * height
         */
        num_chars_current_line = 0, y3 = y2;
        for (int i = completion_buffer_idx; i >= 0; i--) {
            if (completion_buffer[i] == '\n') {
                y3 -= (num_chars_current_line / xs + 1);
                num_chars_current_line = 0;
            } else {
                num_chars_current_line++;
            }
        }
        y3 -= (num_chars_current_line / xs);
        if (y3 < 1) y3 = 1;
        num_completion_lines = y2 - y3;

        /* force the cursor back to pre completion buffer position */
        sprintf(tmp_buffer, "\033[%d;%dH", y3, x1);
        write(STDOUT_FILENO, tmp_buffer, strlen(tmp_buffer));
    }
#endif
}

static int
copilot_on_event() {
#ifdef COPILOT_ENABLED
    /* skip an input line that looks
     * like a slash command
     */
    char *tmp = rl_line_buffer;
    while (tmp && *tmp) {
        switch (*tmp) {
            case ' ':
            case '\t':
                goto end;
            case '\\':
                return 0;
            default:
                break;
        }
        break;
end:
        tmp++;
    }

    if (pthread_mutex_lock(&mtx) != 0) {
        fprintf(stderr, "failed to obtain copilot lock\n");
        return 0;
    }

    if (
        session_primed == false ||
        strncmp(
            input_buffer,
            rl_line_buffer,
            sizeof(input_buffer)
        ) != 0
    ) {
        /* if the input buffer has changed
         * since our last request
         * reset the completion buffer and
         * send prime requests to the server
         */
        completion_buffer_idx = 0;
        last_completion_buffer_idx = 0;
        completion_buffer[0] = '\0';
        strlcpy(
            input_buffer,
            rl_line_buffer,
            sizeof(input_buffer)
        );

        prime = rl_end > 5 || session_primed == false;
        if (prime == true) {
            session_primed = true;
            pxl_tickle();
        } else {
            pxl_cancel_inflight_requests();
        }
    } else if (prime == true && rl_end > 5) {
        /* if the input buffer hasn't changed but
         * we were just priming, then send
         * a completion request
         */
        prime = false;
        pxl_tickle();
    }

    if (pthread_mutex_unlock(&mtx) != 0) {
        fprintf(stderr, "failed to release copilot lock\n");
        return 0;
    }

    if (last_completion_buffer_idx != completion_buffer_idx) {
        /* we have something new in the completion buffer
         * force and update of the display
         */
        rl_forced_update_display();
        last_completion_buffer_idx = completion_buffer_idx;
    }
#endif
    return 0;
}

int
copilot_get_meta(char **session_id, char **schema_buffer) {
    if (pthread_mutex_lock(&mtx) != 0) {
        fprintf(stderr, "failed to obtain copilot lock\n");
        return 1;
    }

    *session_id = sid;
    *schema_buffer = schema->data;

    if (pthread_mutex_unlock(&mtx) != 0) {
        fprintf(stderr, "failed to release copilot lock\n");
        return 1;
    }

    return 0;
}

int
copilot_get_request(
    char **session_id,
    char **schema_buffer,
    char **prompt_buffer,
    bool *is_prime
) {
    if (pthread_mutex_lock(&mtx) != 0) {
        fprintf(stderr, "failed to obtain copilot lock\n");
        return 1;
    }

    resetStringInfo(prompt);
    escape_json(prompt, input_buffer);

    *session_id = sid;
    *schema_buffer = schema->data;
    *prompt_buffer = prompt->data;
    *is_prime = prime;

    if (pthread_mutex_unlock(&mtx) != 0) {
        fprintf(stderr, "failed to release copilot lock\n");
        return 1;
    }

    return 0;
}


void
copilot_on_chat(char *token, bool stop, bool error, bool cancel) {
    if (token != NULL) {
        /*FILE *fd = fopen("/tmp/chat.log", "a");*/
        /*fprintf(fd, "<token>%lu:%s</token>\n", strlen(token), token);*/
        /*fclose(fd);*/
        write(STDOUT_FILENO, token, strlen(token));
    } else if (stop || error || cancel) {
        write(STDOUT_FILENO, "\n", 1);
    }
}


void
copilot_on_completion(char *token, bool stop, bool error, bool cancel) {
    if (token != NULL) {
        /*FILE *fd = fopen("/tmp/completion.log", "a");*/
        /*fprintf(fd, "<token>%lu:%s</token>\n", strlen(token), token);*/
        /*fprintf(*/
        /*    fd,*/
        /*    "<start>%d:%s</start>\n",*/
        /*    completion_buffer_idx,*/
        /*    completion_buffer*/
        /*);*/
        completion_buffer_idx += strlcat(
            completion_buffer + completion_buffer_idx,
            token,
            sizeof(completion_buffer) - completion_buffer_idx
        );
        /*fprintf(*/
        /*    fd,*/
        /*    "<end>%d:%s</end>\n########\n",*/
        /*    completion_buffer_idx,*/
        /*    completion_buffer*/
        /*);*/
        /*fclose(fd);*/
    }
}


static char *
copilot_completion_generator(const char *text, int state) {
    if (state == 0) {
        rl_insert_text(completion_buffer);
        completion_buffer_idx = 0;
        completion_buffer[0] = '\0';
    }
    return NULL;
}


static char **
copilot_completion(const char *text, int start, int end) {
    if (completion_buffer_idx > 0) {
        rl_attempted_completion_over = 1;
        return rl_completion_matches(text, copilot_completion_generator);
    }
    return psql_completion(text, start, end);
}


void
copilot_init() {
#ifdef COPILOT_ENABLED
    schema = makeStringInfo();
    pg_strong_random_init();

    if (!pxl_init()) {
        return;
    }

    prompt = makeStringInfo();
    pthread_mutex_init(&mtx, NULL);

    /* configure readline */
    rl_redisplay_function = copilot_redisplay;
    rl_event_hook = copilot_on_event;
    rl_attempted_completion_function = copilot_completion;
#endif
}


void
copilot_refresh_schema() {
#ifdef COPILOT_ENABLED
    PGresult *res;
    PQExpBuffer buf;

#ifndef COPILOT_DEBUG
    if (!pg_strong_random(sid, SESSION_ID_SIZE)) {
        pg_log_error("failed to generate session id.");
        return;
    }
    for (int i = 0; i < SESSION_ID_SIZE; i++) {
        snprintf(
            sid + i * 2,
            2 * SESSION_ID_SIZE + 1 - i * 2,
            "%02x",
            (unsigned char)sid[i]
        );
    }
#else
    snprintf(sid, sizeof(sid), "test");
#endif
    session_primed = false;

    buf = createPQExpBuffer();

    res = PSQLexec(
        "SELECT table_name\n"
        "FROM information_schema.tables\n"
        "WHERE table_schema = 'public' AND table_type = 'BASE TABLE'\n"
        "ORDER BY table_name"
    );
    if (!res) {
        pg_log_error("failed to refresh schema.");
        return;
    }

    for (int i = 0; i < PQntuples(res); i++) {
        const char *table;
        char qry[1024];
        PGresult *r;

        table = PQgetvalue(res, i, 0);

        snprintf(
            qry,
            sizeof(qry),
            "SELECT column_name, data_type, udt_name, is_nullable, column_default\n"
            "FROM information_schema.columns\n"
            "WHERE table_name = '%s'\n"
            "ORDER BY ordinal_position",
            table
        );

        appendPQExpBuffer(buf, "CREATE TABLE %s (\n", gettext_noop(table));

        r = PSQLexec(qry);
        for (int j = 0; j < PQntuples(r); j++) {
            const char *col, *typ, *utyp;

            col = PQgetvalue(r, j, 0);
            typ = PQgetvalue(r, j, 1);
            if (strcmp(typ, "USER-DEFINED") == 0) {
                utyp = PQgetvalue(r, j, 2);
                appendPQExpBuffer(buf, "\t%s %s", gettext_noop(col), gettext_noop(utyp));
            } else {
                appendPQExpBuffer(buf, "\t%s %s", gettext_noop(col), gettext_noop(typ));
            }
            if (j != PQntuples(r) - 1) {
                appendPQExpBuffer(buf, ",\n");
            }
        }
        PQclear(r);

        appendPQExpBuffer(buf, "\n);\n");
    }

    PQclear(res);

    escape_json(schema, buf->data);

    destroyPQExpBuffer(buf);
#endif
}

/* ughhh
 * i've just copy pasted this from src/backend/utils/adt/json.c
 * it looks like i'm not the first to do this?
 * it's also duplicated in pg_combinebackup & test/modules/test_json_pareser
 * the problem i hit was linking psql against adt in the makefiles.
 * i suspect the other instances had the same problem.
 * ideally this function should get factored out into jsonapi?
 */
static void
escape_json(StringInfo buf, const char *str)
{
    const char *p;

    for (p = str; *p; p++)
    {
        switch (*p)
        {
            case '\b':
                appendStringInfoString(buf, "\\b");
                break;
            case '\f':
                appendStringInfoString(buf, "\\f");
                break;
            case '\n':
                appendStringInfoString(buf, "\\n");
                break;
            case '\r':
                appendStringInfoString(buf, "\\r");
                break;
            case '\t':
                appendStringInfoString(buf, "\\t");
                break;
            case '"':
                appendStringInfoString(buf, "\\\"");
                break;
            case '\\':
                appendStringInfoString(buf, "\\\\");
                break;
            default:
                if ((unsigned char) *p < ' ')
                    appendStringInfo(buf, "\\u%04x", (int) *p);
                else
                    appendStringInfoCharMacro(buf, *p);
                break;
        }
    }
}
