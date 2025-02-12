/* Copyright (C) 2015-2021, Wazuh Inc.
 * Copyright (C) 2009 Trend Micro Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

/* Read MS SQL logs */

#include "shared.h"
#include "logcollector.h"
#include "os_crypto/sha1/sha1_op.h"


/* Send MS SQL message and check the return code */
static void __send_mssql_msg(logreader *lf, int drop_it, char *buffer) {
    mdebug2("Reading MSSQL message: '%s'", buffer);
    if (drop_it == 0) {
        w_msg_hash_queues_push(buffer, lf->file, strlen(buffer) + 1, lf->log_target, LOCALFILE_MQ);
    }
}

/* Read MS SQL log files */
void *read_mssql_log(logreader *lf, int *rc, int drop_it) {
    size_t str_len = 0;
    int need_clear = 0;
    char *p;
    char str[OS_MAXSTR + 1];
    char buffer[OS_MAXSTR + 1];
    int lines = 0;
    /* Zero buffer and str */
    buffer[0] = '\0';
    buffer[OS_MAXSTR] = '\0';
    str[OS_MAXSTR] = '\0';
    *rc = 0;

    /* Obtain context to calculate hash */
    SHA_CTX context;
    int64_t current_position = w_ftell(lf->fp);
    w_get_hash_context(lf->file, &context, current_position);

    /* Get new entry */
    while (can_read() && fgets(str, OS_MAXSTR - OS_LOG_HEADER, lf->fp) != NULL && (!maximum_lines || lines < maximum_lines)) {

        lines++;
        /* Get buffer size */
        str_len = strlen(str);

        OS_SHA1_Stream(&context, NULL, str);

        /* Check str_len size. Very useless, but just to make sure */
        if (str_len >= sizeof(buffer) - 2) {
            str_len = sizeof(buffer) - 10;
        }

        /* Get the last occurrence of \n */
        if ((p = strrchr(str, '\n')) != NULL) {
            *p = '\0';

            /* If need clear is set, we just get the line and ignore it */
            if (need_clear) {
                need_clear = 0;
                continue;
            }
        } else {
            need_clear = 1;
        }

#ifdef WIN32
        if ((p = strrchr(str, '\r')) != NULL) {
            *p = '\0';
        }

        /* Look for empty string (only on windows) */
        if (str_len <= 1) {
            continue;
        }

        /* Windows can have comment on their logs */
        if (str[0] == '#') {
            continue;
        }
#endif

        /* MS SQL messages have the following formats:
         * 2009-03-25 04:47:30.01 Server
         * 2003-10-09 00:00:06.68 sys1
         * 2009-02-06 11:48:59     Server
         */
        if ((str_len > 19) &&
                (str[4] == '-') &&
                (str[7] == '-') &&
                (str[10] == ' ') &&
                (str[13] == ':') &&
                (str[16] == ':') &&
                isdigit((int)str[0]) &&
                isdigit((int)str[1]) &&
                isdigit((int)str[2]) &&
                isdigit((int)str[3])) {

            /* If the saved message is empty, set it and continue */
            if (buffer[0] == '\0') {
                strncpy(buffer, str, str_len + 2);
                continue;
            }

            /* If not, send the saved one and store the new one for later */
            else {
                __send_mssql_msg(lf, drop_it, buffer);

                /* Store current one at the buffer */
                strncpy(buffer, str, str_len + 2);
            }
        }

        /* Query logs can be in multiple lines
         * They always start with a tab in the additional lines
         */
        else if ((str_len > 2) && (buffer[0] != '\0')) {
            /* Size of the buffer */
            size_t buffer_len = strlen(buffer);

            p = str;

            /* Remove extra spaces and tabs */
            while (*p == ' ' || *p == '\t') {
                p++;
            }

            /* Add additional message to the saved buffer */
            if (sizeof(buffer) - buffer_len > str_len + 256) {
                /* Here we make sure that the size of the buffer
                 * minus what was used (strlen) is greater than
                 * the length of the received message.
                 */
                buffer[buffer_len] = ' ';
                buffer[buffer_len + 1] = '\0';
                strncat(buffer, str, str_len + 3);
            }
        }
    }

    current_position = w_ftell(lf->fp);
    w_update_file_status(lf->file, current_position, &context);

    /* Send whatever is stored */
    if (buffer[0] != '\0') {
        __send_mssql_msg(lf, drop_it, buffer);
    }

    mdebug2("Read %d lines from %s", lines, lf->file);
    return (NULL);
}
