#include <stdio.h>
#include <pthread.h>
#include <unistd.h>


#include "postgres_fe.h"

#include "common.h"
#include "common/jsonapi.h"
#include "common/logging.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "copilot.h"
#include "sse.h"
#include "pxl.h"

#ifdef HAVE_COPILOT
#include <curl/curl.h>
#endif


#ifndef WIN32
#define PXLCFG        ".pxl-psql"
#else
#define PXLCFG        "pxl-psql.conf"
#endif

#define PXL_BUFFER_SIZE 40*1024

typedef enum {
    PXL_CFG_PARSE_STATE_INIT,
    PXL_CFG_PARSE_STATE_API_URL,
    PXL_CFG_PARSE_STATE_ACCESS_TOKEN,
    PXL_CFG_PARSE_STATE_API_KEY,
} pxl_cfg_parse_state;

typedef struct {
    pxl_cfg_parse_state s;
    bool is_authenticated;
    char *api_url;
    char *access_token;
    char *api_key;
} pxl_cfg;

typedef enum {
    PXL_DEVICE_CODE_PARSE_STATE_INIT,
    PXL_DEVICE_CODE_PARSE_STATE_DEVICE_CODE,
    PXL_DEVICE_CODE_PARSE_STATE_USER_CODE,
    PXL_DEVICE_CODE_PARSE_STATE_URL,
} pxl_device_code_parse_state;

typedef struct {
    pxl_device_code_parse_state s;
    char *device_code;
    char *user_code;
    char *url;
} pxl_device_code;

typedef struct {
    bool is_token;
    char *access_token;
} pxl_access_token;

static pthread_t id;
static pthread_cond_t sig;
static pthread_mutex_t mtx;
static pxl_cfg cfg;
static int req_id = 0;

static const char *default_api_url = "https://api.pxlapp.com";
static const char *client_id = "uYhVaupkEe6f4vpbEO6DFQ==";


static size_t
on_sse_data(char *ptr, size_t s, size_t nb, void *userdata) {
    if (userdata != NULL) {
        sse *state = (sse *)userdata;

        // cancel request if ids don't match
        if (state->id != req_id) {
            return 0;
        }

        sse_parse(state, ptr, nb);
    }
    return s * nb;
}


static void *
pxl_run(void *params) {
#ifdef HAVE_COPILOT
    char *session_id, *prompt;
    bool is_prime;
    StringInfo auth_header;
    CURL *c = NULL;
    CURLM *m = NULL;
    CURLMcode mc;
    struct curl_slist *headers;
    int current_req_id, last_req_id, num_running;
    char url_buffer[1024];
    char request_buffer[PXL_BUFFER_SIZE];

    auth_header = makeStringInfo();
    m = curl_multi_init();
    current_req_id = 0;
    last_req_id = 0;
    while (true) {
        if (last_req_id == req_id) {
            if (0 != pthread_cond_wait(&sig, &mtx)) {
                pg_log_error("failed to wait for lock.");
                return NULL;
            }
        }

        /* new request required */
        if (cfg.is_authenticated == true && current_req_id != req_id) {
            /* cancel and clean up any running requests */
            if (c != NULL) {
                curl_multi_remove_handle(m, c);
                curl_easy_cleanup(c);
            }

            c = curl_easy_init();
            if (!c) {
                pg_log_error("failed to start curl.");
                return NULL;
            }

            last_req_id = current_req_id;
            current_req_id = req_id;

            if (copilot_get_request(&session_id, &prompt, &is_prime) != 0) {
                pg_log_error("failed to get request.");
                return NULL;
            }

            snprintf(
                request_buffer,
                sizeof(request_buffer),
                "{\"sessionId\":\"%s\",\"requestId\":\"%d\",\"prompt\":\"%s\",\"maxTokens\":%d}",
                session_id,
                current_req_id,
                prompt,
                is_prime == true ? 0 : 256
            );

            /* the curl_easy_cleanup call above has already
             * freed the memory allocated on the previous call
             */
            resetStringInfo(auth_header);
            if (cfg.access_token != NULL) {
                appendStringInfo(auth_header, "Authorization: Bearer %s", cfg.access_token);
            } else if (cfg.api_key != NULL) {
                appendStringInfo(auth_header, "API-KEY: %s", cfg.api_key);
            }

            headers = curl_slist_append(
                NULL,
                auth_header->data
            );
            headers = curl_slist_append(
                headers,
                "Content-Type: application/json"
            );

            snprintf(url_buffer, sizeof(url_buffer), "%s/v1/sql/completion", cfg.api_url);
            curl_easy_setopt(c, CURLOPT_URL, url_buffer);
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_sse_data);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, sse_init(current_req_id, copilot_on_completion));
            curl_easy_setopt(c, CURLOPT_POST, 1);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, request_buffer);
            curl_easy_setopt(c, CURLOPT_VERBOSE, 0);
            curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1);
            curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1);
            curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 1);

            curl_multi_add_handle(m, c);
        }

        mc = curl_multi_perform(m, &num_running);
        if (mc == CURLM_OK) {
            if (num_running == 0) {
                sse *s = NULL;
                last_req_id = current_req_id;
                curl_multi_remove_handle(m, c);
                curl_easy_getinfo(c, CURLOPT_WRITEDATA, &s);
                if (s != NULL) {
                    sse_clean(s);
                }
                curl_easy_cleanup(c);
                c = NULL;
                continue;
            }
            curl_multi_poll(m, NULL, 0, 100, NULL);
        }
    }
#endif

    return NULL;
}



static JsonParseErrorType
cfg_json_key(void *state, char *fname, bool isnull) {
    pxl_cfg *cfg = state;
    if (strcmp(fname, "apiUrl") == 0) {
        cfg->s = PXL_CFG_PARSE_STATE_API_URL;
    } else if (strcmp(fname, "accessToken") == 0) {
        cfg->s = PXL_CFG_PARSE_STATE_ACCESS_TOKEN;
    } else if (strcmp(fname, "apiKey") == 0) {
        cfg->s = PXL_CFG_PARSE_STATE_API_KEY;
    } else {
        cfg->s = PXL_CFG_PARSE_STATE_INIT;
    }
    return JSON_SUCCESS;
}


static JsonParseErrorType
cfg_json_value(void *state, char *token, JsonTokenType token_type) {
    pxl_cfg *cfg = state;
    switch (cfg->s) {
        case PXL_CFG_PARSE_STATE_API_URL:
            cfg->api_url = strdup(token);
            break;
        case PXL_CFG_PARSE_STATE_ACCESS_TOKEN:
            cfg->access_token = strdup(token);
            break;
        case PXL_CFG_PARSE_STATE_API_KEY:
            cfg->api_key = strdup(token);
            break;
        default:
            break;
    }
    return JSON_SUCCESS;
}


static JsonParseErrorType
device_code_json_key(void *state, char *fname, bool isnull) {
    pxl_device_code *dc = state;
    if (strcmp(fname, "device_code") == 0) {
        dc->s = PXL_DEVICE_CODE_PARSE_STATE_DEVICE_CODE;
    } else if (strcmp(fname, "user_code") == 0) {
        dc->s = PXL_DEVICE_CODE_PARSE_STATE_USER_CODE;
    } else if (strcmp(fname, "verification_uri") == 0) {
        dc->s = PXL_DEVICE_CODE_PARSE_STATE_URL;
    } else {
        dc->s = PXL_DEVICE_CODE_PARSE_STATE_INIT;
    }
    return JSON_SUCCESS;
}


static JsonParseErrorType
device_code_json_value(void *state, char *token, JsonTokenType token_type) {
    pxl_device_code *dc = state;
    switch (dc->s) {
        case PXL_DEVICE_CODE_PARSE_STATE_DEVICE_CODE:
            dc->device_code = strdup(token);
            break;
        case PXL_DEVICE_CODE_PARSE_STATE_USER_CODE:
            dc->user_code = strdup(token);
            break;
        case PXL_DEVICE_CODE_PARSE_STATE_URL:
            dc->url = strdup(token);
            break;
        default:
            break;
    }
    return JSON_SUCCESS;
}


static JsonParseErrorType
access_token_json_key(void *state, char *fname, bool isnull) {
    pxl_access_token *at = state;
    at->is_token = (strcmp(fname, "access_token") == 0);
    return JSON_SUCCESS;
}


static JsonParseErrorType
access_token_json_value(void *state, char *token, JsonTokenType token_type) {
    pxl_access_token *at = state;
    if (at->is_token == true) {
        at->access_token = token;
    }
    return JSON_SUCCESS;
}


static size_t
pxl_on_data_auth_me(char *ptr, size_t s, size_t nb, void *userdata) {
    return s * nb;
}


static size_t
pxl_on_data_oauth(char *ptr, size_t s, size_t nb, void *userdata) {
    appendBinaryStringInfo((StringInfo)userdata, ptr, nb);
    return s * nb;
}


static int
pxl_authenticate() {
    FILE *fd;
    char home[MAXPGPATH];
    char cfg_path[MAXPGPATH];
    char url_buffer[1024];
    char *_client_id, *_device_code;
    StringInfo tmp_buffer;
    StringInfo res_buffer;
    JsonLexContext *lex;
    JsonSemAction sem;
    JsonParseErrorType json_error;
    CURL *c;
    CURLcode res;
    long http_status;
    struct curl_slist *headers;
    pxl_device_code dc;
    pxl_access_token at;

    /* ensure cfg is reset */
    memset(&cfg, 0, sizeof(cfg));
    memset(&dc, 0, sizeof(dc));
    memset(&at, 0, sizeof(at));

    /* set default api_url */
    cfg.api_url = (char *)default_api_url;

    /* obtain path to home directory */
    if (!get_home_path(home)) {
        pg_log_error("failed to get home path.");
        return 1;
    }

    tmp_buffer = makeStringInfo();

    snprintf(cfg_path, MAXPGPATH, "%s/%s", home, PXLCFG);
    fd = fopen(cfg_path, "rb");
    if (fd == NULL) {
        pg_log_error("failed to open %s", PXLCFG);
        goto request_auth;
    }

    if (tmp_buffer == NULL) {
        pg_log_error("failed to allocate buffer for %s.", PXLCFG);
        return 1;
    }

    while (true) {
        int r;
        char tmp[1024];
        r = fread(tmp, sizeof(char), sizeof(tmp), fd);
        if (r <= 0) {
            break;
        }
        appendBinaryStringInfo(tmp_buffer, tmp, r);
    }
    fclose(fd);

    lex = makeJsonLexContextCstringLen(NULL, tmp_buffer->data, tmp_buffer->len, PG_UTF8, true);
    memset(&sem, 0, sizeof(sem));
    sem.semstate = &cfg;
    sem.object_field_start = cfg_json_key;
    sem.scalar = cfg_json_value;

    json_error = pg_parse_json(lex, &sem);
    freeJsonLexContext(lex);

    if (json_error != JSON_SUCCESS) {
        pg_log_error("failed to parse %s.", PXLCFG);
        destroyStringInfo(tmp_buffer);
        return 1;
    }

    /* reset api url if required */
    if (cfg.api_url == NULL) {
        cfg.api_url = (char *)default_api_url;
    }

    if (cfg.access_token == NULL && cfg.api_key == NULL) {
        goto request_auth;
    }

    c = curl_easy_init();
    if (c == NULL) {
        pg_log_error("failed create curl request.");
        destroyStringInfo(tmp_buffer);
        return 1;
    }

    resetStringInfo(tmp_buffer);
    if (cfg.access_token != NULL) {
        appendStringInfo(tmp_buffer, "Authorization: Bearer %s", cfg.access_token);
    } else if (cfg.api_key != NULL) {
        appendStringInfo(tmp_buffer, "API-KEY: %s", cfg.api_key);
    }

    headers = curl_slist_append(
        NULL,
        tmp_buffer->data
    );

    snprintf(url_buffer, sizeof(url_buffer), "%s/v1/auth/me", cfg.api_url);
    curl_easy_setopt(c, CURLOPT_URL, url_buffer);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, pxl_on_data_auth_me);
    curl_easy_setopt(c, CURLOPT_VERBOSE, 0);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 1);

    res = curl_easy_perform(c);
    if (res != CURLE_OK) {
        pg_log_error("failed to create authentication request.");
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
        goto request_auth;
    }

    res = curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_status);
    if (res != CURLE_OK) {
        pg_log_error("failed to get authentication status.");
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
        goto request_auth;
    }

    curl_easy_cleanup(c);

    if (http_status != 200) {
        pg_log_error("failed to authenticate.");
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
        goto request_auth;
    }

    destroyStringInfo(tmp_buffer);
    cfg.is_authenticated = true;
    return 0;

request_auth:
    /*pg_log_error("authentication required.");*/
    res_buffer = makeStringInfo();
    resetStringInfo(tmp_buffer);

    c = curl_easy_init();
    if (c == NULL) {
        pg_log_error("failed create curl request.");
        destroyStringInfo(tmp_buffer);
        destroyStringInfo(res_buffer);
        return 1;
    }

    headers = curl_slist_append(
        NULL,
        "Content-Type: application/x-www-form-urlencoded"
    );

    _client_id = curl_easy_escape(c, client_id, strlen(client_id));

    appendStringInfo(tmp_buffer, "client_id=%s", client_id);

    snprintf(url_buffer, sizeof(url_buffer), "%s/v1/oauth/device/code", cfg.api_url);
    curl_easy_setopt(c, CURLOPT_URL, url_buffer);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, pxl_on_data_oauth);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, res_buffer);
    curl_easy_setopt(c, CURLOPT_POST, 1);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, tmp_buffer->data);
    curl_easy_setopt(c, CURLOPT_VERBOSE, 0);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 1);

    res = curl_easy_perform(c);
    if (res != CURLE_OK) {
        pg_log_error("failed to create oauth device code request.");
        destroyStringInfo(tmp_buffer);
        destroyStringInfo(res_buffer);
        curl_free(_client_id);
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
        return 1;
    }

    res = curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_status);
    if (res != CURLE_OK) {
        pg_log_error("failed to get oauth device code status.");
        destroyStringInfo(tmp_buffer);
        destroyStringInfo(res_buffer);
        curl_free(_client_id);
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
        return 1;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (http_status != 200) {
        pg_log_error("failed to get oauth device code.");
        destroyStringInfo(tmp_buffer);
        destroyStringInfo(res_buffer);
        curl_free(_client_id);
        return 1;
    }

    lex = makeJsonLexContextCstringLen(NULL, res_buffer->data, res_buffer->len, PG_UTF8, true);
    memset(&sem, 0, sizeof(sem));
    sem.semstate = &dc;
    sem.object_field_start = device_code_json_key;
    sem.scalar = device_code_json_value;

    json_error = pg_parse_json(lex, &sem);
    freeJsonLexContext(lex);

    fprintf(
        stdout,
        "Please browse to \033[1m%s\033[0m and enter the following one time code:\n\033[1m%s\033[0m\n",
        dc.url,
        dc.user_code
    );
    fflush(stdout);

    /* wait in loop for authentication */
    for (int i = 0; i < 10; i++) {
        pg_log_error("waiting for authorization...");
        if (sleep(20) != 0) {
            pg_log_error("interrupted.");
            destroyStringInfo(tmp_buffer);
            destroyStringInfo(res_buffer);
            curl_free(_client_id);
            return 1;
        }

        c = curl_easy_init();
        if (c == NULL) {
            pg_log_error("failed create curl request.");
            destroyStringInfo(tmp_buffer);
            destroyStringInfo(res_buffer);
            curl_free(_client_id);
            return 1;
        }

        headers = curl_slist_append(
            NULL,
            "Content-Type: application/x-www-form-urlencoded"
        );

        resetStringInfo(tmp_buffer);
        resetStringInfo(res_buffer);

        _device_code = curl_easy_escape(c, dc.device_code, strlen(dc.device_code));
        appendStringInfo(
            tmp_buffer,
            "client_id=%s&device_code=%s&grant_type=urn:ietf:params:oauth:grant-type:device_code",
            _client_id,
            _device_code
        );
        curl_free(_device_code);

        snprintf(url_buffer, sizeof(url_buffer), "%s/v1/oauth/access_token", cfg.api_url);
        curl_easy_setopt(c, CURLOPT_URL, url_buffer);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, pxl_on_data_oauth);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, res_buffer);
        curl_easy_setopt(c, CURLOPT_POST, 1);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, tmp_buffer->data);
        curl_easy_setopt(c, CURLOPT_VERBOSE, 0);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 1);

        res = curl_easy_perform(c);
        if (res != CURLE_OK) {
            pg_log_error("failed to create oauth device code request.");
            curl_free(_client_id);
            curl_easy_cleanup(c);
            continue;
        }

        res = curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_status);
        if (res != CURLE_OK) {
            pg_log_error("failed to get oauth device code status.");
            curl_free(_client_id);
            curl_slist_free_all(headers);
            curl_easy_cleanup(c);
            continue;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(c);

        if (http_status != 200) {
            continue;
        }

        /* parse the response */
        lex = makeJsonLexContextCstringLen(NULL, res_buffer->data, res_buffer->len, PG_UTF8, true);
        memset(&sem, 0, sizeof(sem));
        sem.semstate = &at;
        sem.object_field_start = access_token_json_key;
        sem.scalar = access_token_json_value;

        json_error = pg_parse_json(lex, &sem);
        freeJsonLexContext(lex);

        if (json_error != JSON_SUCCESS) {
            pg_log_error("failed to parse access_token.");
            destroyStringInfo(tmp_buffer);
            destroyStringInfo(res_buffer);
            curl_free(_client_id);
            return 1;
        }

        /* write out access_token file */
        resetStringInfo(tmp_buffer);
        appendStringInfo(
            tmp_buffer,
            "{\n\t\"apiUrl\":\"%s\",\n\t\"accessToken\":\"%s\"\n}",
            cfg.api_url,
            at.access_token
        );

        fd = fopen(cfg_path, "wb");
        if (fd == NULL) {
            pg_log_error("failed to open %s for writing.", PXLCFG);
            destroyStringInfo(tmp_buffer);
            destroyStringInfo(res_buffer);
            curl_free(_client_id);
            return 1;
        }

        if (
            fwrite(
                tmp_buffer->data,
                sizeof(char),
                tmp_buffer->len,
                fd
            ) != tmp_buffer->len
        ) {
            pg_log_error("failed to write %s.", PXLCFG);
        } else {
            if (cfg.access_token != NULL) {
                free(cfg.access_token);
            }
            if (cfg.api_key != NULL) {
                free(cfg.api_key);
            }
            cfg.access_token = strdup(at.access_token);
            cfg.is_authenticated = true;
        }

        fclose(fd);

        destroyStringInfo(tmp_buffer);
        destroyStringInfo(res_buffer);
        curl_free(_client_id);
        return 0;
    }

    pg_log_error("failed to get oauth device code status.");

    destroyStringInfo(tmp_buffer);
    destroyStringInfo(res_buffer);
    curl_free(_client_id);

    return 1;
}


bool
pxl_init() {
#ifdef HAVE_COPILOT
    curl_global_init(CURL_GLOBAL_ALL);

    /* authenticate */
    if (pxl_authenticate() != 0) {
        return false;
    }

    /* start the autocomplete thread */
    pthread_cond_init(&sig, NULL);
    pthread_mutex_init(&mtx, NULL);
    pthread_create(&id, NULL, pxl_run, NULL);

    return true;
#endif
}

void
pxl_tickle() {
    req_id++;
    pthread_cond_signal(&sig);
}

void
pxl_cancel_inflight_requests() {
    req_id++;
}

void
pxl_session(char *session_id, char *schema) {
    CURL *c;
    CURLcode res;
    long http_status;
    struct curl_slist *headers;
    StringInfo auth_header;
    char url_buffer[1024];
    char request_buffer[PXL_BUFFER_SIZE];
    int rid = ++req_id;

    if (cfg.is_authenticated != true) {
        printf("Unable to run ai commands. Authentication required.\n");
        return;
    }

    auth_header = makeStringInfo();
    if (cfg.access_token != NULL) {
        appendStringInfo(auth_header, "Authorization: Bearer %s", cfg.access_token);
    } else if (cfg.api_key != NULL) {
        appendStringInfo(auth_header, "API-KEY: %s", cfg.api_key);
    }

    headers = curl_slist_append(
        NULL,
        auth_header->data
    );
    headers = curl_slist_append(
        headers,
        "Content-Type: application/json"
    );

    snprintf(
        request_buffer,
        sizeof(request_buffer),
        "{\"sessionId\":\"%s\",\"schema\":\"%s\"}",
        session_id,
        schema
    );

    c = curl_easy_init();
    if (c != NULL) {
        snprintf(url_buffer, sizeof(url_buffer), "%s/v1/sql/session", cfg.api_url);
        curl_easy_setopt(c, CURLOPT_URL, url_buffer);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_sse_data);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, sse_init(rid, copilot_on_chat));
        curl_easy_setopt(c, CURLOPT_POST, 1);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, request_buffer);
        curl_easy_setopt(c, CURLOPT_VERBOSE, 0);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 1);

        res = curl_easy_perform(c);
        if (res != CURLE_OK) {
            pg_log_error("failed to create pxl session.");
            goto end;
        }

        res = curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_status);
        if (res != CURLE_OK) {
            pg_log_error("failed to create pxl session.");
            goto end;
        }

        if (http_status != 200) {
            pg_log_error("ai failed.");
        }
    }

end:
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
}

void
pxl_chat(char *session_id, char *query) {
    CURL *c;
    CURLcode res;
    long http_status;
    struct curl_slist *headers;
    StringInfo auth_header;
    char url_buffer[1024];
    char request_buffer[PXL_BUFFER_SIZE];
    int rid = ++req_id;

    if (cfg.is_authenticated != true) {
        printf("Unable to run ai commands. Authentication required.\n");
        return;
    }

    auth_header = makeStringInfo();
    if (cfg.access_token != NULL) {
        appendStringInfo(auth_header, "Authorization: Bearer %s", cfg.access_token);
    } else if (cfg.api_key != NULL) {
        appendStringInfo(auth_header, "API-KEY: %s", cfg.api_key);
    }

    headers = curl_slist_append(
        NULL,
        auth_header->data
    );
    headers = curl_slist_append(
        headers,
        "Content-Type: application/json"
    );

    snprintf(
        request_buffer,
        sizeof(request_buffer),
        "{\"sessionId\":\"%s\",\"requestId\":\"%d\",\"prompt\":\"%s\"}",
        session_id,
        rid,
        query
    );


    c = curl_easy_init();
    if (c != NULL) {
        snprintf(url_buffer, sizeof(url_buffer), "%s/v1/sql/chat", cfg.api_url);
        curl_easy_setopt(c, CURLOPT_URL, url_buffer);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_sse_data);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, sse_init(rid, copilot_on_chat));
        curl_easy_setopt(c, CURLOPT_POST, 1);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, request_buffer);
        curl_easy_setopt(c, CURLOPT_VERBOSE, 0);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 1);

        res = curl_easy_perform(c);
        if (res != CURLE_OK) {
            pg_log_error("failed to create chat request.");
            goto end;
        }

        res = curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_status);
        if (res != CURLE_OK) {
            pg_log_error("failed to create chat status.");
            goto end;
        }

        if (http_status != 200) {
            pg_log_error("ai failed %ld.", http_status);
        }
    }

end:
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
}
