/* Copyright (C) 2015-2021, Wazuh Inc.
 * Copyright (C) 2009 Trend Micro Inc.
 * All right reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#include "shared.h"
#include "syscheck.h"
#include "syscheck_op.h"
#include "integrity_op.h"
#include "time_op.h"
#include "db/fim_db_files.h"
#include "db/fim_db_registries.h"
#include "registry/registry.h"

#ifdef WAZUH_UNIT_TESTING
/* Remove static qualifier when unit testing */
#define static

/* Replace assert with mock_assert */
extern void mock_assert(const int result, const char* const expression,
                        const char * const file, const int line);

#undef assert
#define assert(expression) \
    mock_assert((int)(expression), #expression, __FILE__, __LINE__);
#endif

// Global variables
static int _base_line = 0;

static fim_state_db _db_state = FIM_STATE_DB_EMPTY;

static const char *FIM_EVENT_TYPE[] = {
    "added",
    "deleted",
    "modified"
};

static const char *FIM_EVENT_MODE[] = {
    "scheduled",
    "realtime",
    "whodata"
};

static cJSON *_fim_file(const char *file, fim_element *item, whodata_evt *w_evt);

#ifndef WIN32
static cJSON *_fim_file_force_update(const char *path, const fim_element *item, const fim_entry *saved);
#endif

void fim_delete_file_event(fdb_t *fim_sql, fim_entry *entry, pthread_mutex_t *mutex,
                           __attribute__((unused))void *alert,
                           __attribute__((unused))void *fim_ev_mode,
                           __attribute__((unused))void *w_evt) {
    int *send_alert = (int *) alert;
    fim_event_mode mode = (fim_event_mode) fim_ev_mode;
    cJSON *json_event = NULL;
    int pos = -1;

    pos = fim_configuration_directory(entry->file_entry.path);

    if(pos == -1) {
        mdebug2(FIM_DELETE_EVENT_PATH_NOCONF, entry->file_entry.path);
        return;
    }
    /* Don't send alert if received mode and mode in configuration aren't the same.
       Scheduled mode events must always be processed to preserve the state of the agent's DB.
    */
    switch (mode) {
    case FIM_REALTIME:
        if (!(syscheck.opts[pos] & REALTIME_ACTIVE)) {
            return;
        }
        break;

    case FIM_WHODATA:
        if (!(syscheck.opts[pos] & WHODATA_ACTIVE)) {
            return;
        }
        break;

    default:
        break;
    }

    if (syscheck.opts[pos] & CHECK_SEECHANGES) {
        fim_diff_process_delete_file(entry->file_entry.path);
    }

    // Remove path from the DB.
    w_mutex_lock(mutex);
    if (fim_db_remove_path(fim_sql, entry->file_entry.path) == FIMDB_ERR) {
        w_mutex_unlock(mutex);
        return;
    }

    if (send_alert) {
        json_event =
        fim_json_event(entry->file_entry.path, NULL, entry->file_entry.data, pos, FIM_DELETE, mode, w_evt, NULL);
    }
    w_mutex_unlock(mutex);

    if (json_event != NULL) {
        mdebug2(FIM_FILE_MSG_DELETE, entry->file_entry.path);
        send_syscheck_msg(json_event);
    }

    cJSON_Delete(json_event);
}


time_t fim_scan() {
    int it = 0;
    struct timespec start;
    struct timespec end;
    time_t end_of_scan;
    clock_t cputime_start;
    int nodes_count = 0;
    struct fim_element item;
    char *real_path = NULL;
    cputime_start = clock();
    gettime(&start);
    minfo(FIM_FREQUENCY_STARTED);
    fim_send_scan_info(FIM_SCAN_START);


    fim_diff_folder_size();
    syscheck.disk_quota_full_msg = true;

    mdebug2(FIM_DIFF_FOLDER_SIZE, DIFF_DIR, syscheck.diff_folder_size);

    w_mutex_lock(&syscheck.fim_scan_mutex);

    w_mutex_lock(&syscheck.fim_entry_mutex);
    fim_db_set_all_unscanned(syscheck.database);
    w_mutex_unlock(&syscheck.fim_entry_mutex);

    while (syscheck.dir[it] != NULL) {
        memset(&item, 0, sizeof(fim_element));
        item.mode = FIM_SCHEDULED;
        item.index = it;
        real_path = fim_get_real_path(it);

        fim_checker(real_path, &item, NULL, 1);
#ifndef WIN32
        if (syscheck.opts[it] & REALTIME_ACTIVE) {
            realtime_adddir(real_path, 0, (syscheck.opts[it] & CHECK_FOLLOW) ? 1 : 0);
        }
#endif
        free(real_path);
        it++;
    }

    w_mutex_unlock(&syscheck.fim_scan_mutex);


#ifdef WIN32
    fim_registry_scan();
#endif
    if (syscheck.file_limit_enabled) {
        w_mutex_lock(&syscheck.fim_entry_mutex);
        nodes_count = fim_db_get_count_entries(syscheck.database);
        w_mutex_unlock(&syscheck.fim_entry_mutex);
    }

    check_deleted_files();

    if (syscheck.file_limit_enabled && (nodes_count >= syscheck.file_limit)) {
        it = 0;

        w_mutex_lock(&syscheck.fim_scan_mutex);

        while ((syscheck.dir[it] != NULL) && (!syscheck.database->full)) {
            memset(&item, 0, sizeof(fim_element));
            item.mode = FIM_SCHEDULED;
            item.index = it;
            real_path = fim_get_real_path(it);

            fim_checker(real_path, &item, NULL, 0);
            free(real_path);
            it++;
        }

        w_mutex_unlock(&syscheck.fim_scan_mutex);

#ifdef WIN32
        if (!syscheck.database->full) {
            fim_registry_scan();
        }
#endif
    }

    gettime(&end);
    end_of_scan = time(NULL);

    if (syscheck.file_limit_enabled) {
        mdebug2(FIM_FILE_LIMIT_VALUE, syscheck.file_limit);
        fim_check_db_state();
    }
    else {
        mdebug2(FIM_FILE_LIMIT_UNLIMITED);
    }

    if (_base_line == 0) {
        _base_line = 1;
    }
    else {
        // In the first scan, the fim inicialization is different between Linux and Windows.
        // Realtime watches are set after the first scan in Windows.
        if (syscheck.realtime != NULL) {
            if (syscheck.realtime->queue_overflow) {
                realtime_sanitize_watch_map();
                syscheck.realtime->queue_overflow = false;
            }
            mdebug2(FIM_NUM_WATCHES, syscheck.realtime->dirtb->elements);
        }
    }

    minfo(FIM_FREQUENCY_ENDED);
    fim_send_scan_info(FIM_SCAN_END);

    if (isDebug()) {
        fim_print_info(start, end, cputime_start); // LCOV_EXCL_LINE
    }
    return end_of_scan;
}

void fim_checker(const char *path, fim_element *item, whodata_evt *w_evt, int report) {
    int node;
    int depth;

#ifdef WIN32
    // Ignore the recycle bin.
    if (check_removed_file(path)){
        return;
    }
#endif

    if (item->mode == FIM_SCHEDULED) {
        // If the directory have another configuration will come back
        if (node = fim_configuration_directory(path), node < 0 || item->index != node) {
            return;
        }
    } else {
        if (node = fim_configuration_directory(path), node < 0) {
            return;
        }
    }

    // We need to process every event generated by scheduled scans because we need to
    // alert about discarded events of real-time and Whodata mode
    if (item->mode != FIM_SCHEDULED && item->mode != FIM_MODE(syscheck.opts[node])) {
        return;
    }

    depth = fim_check_depth(path, node);

    if (depth > syscheck.recursion_level[node]) {
        mdebug2(FIM_MAX_RECURSION_LEVEL, depth, syscheck.recursion_level[node], path);
        return;
    }

    item->index = node;
    item->configuration = syscheck.opts[node];
    fim_entry *saved_entry = NULL;

    // Deleted file. Sending alert.
    if (w_stat(path, &(item->statbuf)) == -1) {
        if(errno != ENOENT) {
            mdebug1(FIM_STAT_FAILED, path, errno, strerror(errno));
            return;
        }

        w_mutex_lock(&syscheck.fim_entry_mutex);
        saved_entry = fim_db_get_path(syscheck.database, path);
        w_mutex_unlock(&syscheck.fim_entry_mutex);

        if (saved_entry) {
            fim_delete_file_event(syscheck.database, saved_entry, &syscheck.fim_entry_mutex, (void *) (int) true,
                                 (void *) (fim_event_mode) item->mode, (void *) w_evt);
            free_entry(saved_entry);
            saved_entry = NULL;
        } else if (item->configuration & CHECK_SEECHANGES) {
            fim_diff_process_delete_file(path);
        }

        return;
    }

#ifdef WIN_WHODATA
    if (w_evt && w_evt->scan_directory == 1) {
        if (w_update_sacl(path)) {
            mdebug1(FIM_SCAL_NOREFRESH, path);
        }
    }
#endif

    if (HasFilesystem(path, syscheck.skip_fs)) {
        return;
    }

    switch(item->statbuf.st_mode & S_IFMT) {
#ifndef WIN32
    case FIM_LINK:
        // Fallthrough
#endif
    case FIM_REGULAR:
        if (fim_check_ignore(path) == 1) {
            return;
        }

        if (fim_check_restrict (path, syscheck.filerestrict[item->index]) == 1) {
            return;
        }

        check_max_fps();

        fim_file(path, item, w_evt, report);
        break;

    case FIM_DIRECTORY:
        if (depth == syscheck.recursion_level[node]) {
            mdebug2(FIM_DIR_RECURSION_LEVEL, path, depth);
            return;
        }
#ifndef WIN32
        if (item->configuration & REALTIME_ACTIVE) {
            realtime_adddir(path, 0, (item->configuration & CHECK_FOLLOW) ? 1 : 0);
        }
#endif
        fim_directory(path, item, w_evt, report);
        break;
    }
}


int fim_directory (const char *dir, fim_element *item, whodata_evt *w_evt, int report) {
    DIR *dp;
    struct dirent *entry;
    char *f_name;
    char *s_name;
    size_t path_size;

    if (!dir) {
        merror(NULL_ERROR);
        return OS_INVALID;
    }

    // Open the directory given
    dp = opendir(dir);

    if (!dp) {
        mwarn(FIM_PATH_NOT_OPEN, dir, strerror(errno));
        return OS_INVALID;
    }

    os_calloc(PATH_MAX + 2, sizeof(char), f_name);
    while ((entry = readdir(dp)) != NULL) {
        // Ignore . and ..
        if ((strcmp(entry->d_name, ".") == 0) ||
                (strcmp(entry->d_name, "..") == 0)) {
            continue;
        }

        strncpy(f_name, dir, PATH_MAX);
        path_size = strlen(dir);
        s_name = f_name + path_size;

        // Check if the file name is already null terminated
        if (*(s_name - 1) != PATH_SEP) {
            *s_name++ = PATH_SEP;
        }
        *(s_name) = '\0';
        strncpy(s_name, entry->d_name, PATH_MAX - path_size - 2);

#ifdef WIN32
        str_lowercase(f_name);
#endif
        // Process the event related to f_name
        fim_checker(f_name, item, w_evt, report);
    }

    os_free(f_name);
    closedir(dp);
    return 0;
}

#ifndef WIN32
/**
 * @brief Processes a file by extracting its information from the DB.
 *
 * @param path The path to the file being processed.
 * @param stack A list used as a stack to store paths to stored inodes that have a conflict with the analysed file.
 * @param tree A tree that helps prevent duplicate entries from being added to the stack.
 * @param event In case the processed file generates and event, it's returned here.
 * @return A fim_sanitize_state_t value representing how the operation ended.
 * @retval FIM_FILE_UPDATED The file has been updated correctly in the DB.
 * @retval FIM_FILE_DELETED The file has been deleted from the DB.
 * @retval FIM_FILE_ADDED_PATHS A collision was detected with provided inode, the paths gotten from the conflicting inode are added to `stack`.
 * @retval FIM_FILE_ERROR An error occured while processing the file.
 */
static fim_sanitize_state_t fim_process_file_from_db(const char *path, OSList *stack, rb_tree *tree, cJSON **event) {
    fim_element item;
    fim_entry *entry;

    assert(path != NULL);
    assert(stack != NULL);
    assert(tree != NULL);
    assert(event != NULL);

    entry = fim_db_get_path(syscheck.database, path);
    if (entry == NULL) {
        // We didn't get an entry
        return FIM_FILE_ERROR;
    }

    // Ensure 'item' does not have garbage in case we need it later
    memset(&item, 0, sizeof(fim_element));

    if (w_stat(entry->file_entry.path, &item.statbuf) == -1) {
        int configuration;

        if (errno != ENOENT) {
            mdebug1(FIM_STAT_FAILED, entry->file_entry.path, errno, strerror(errno));
            free_entry(entry);
            return FIM_FILE_ERROR;
        }

        configuration = fim_configuration_directory(path);
        if (configuration == -1) {
            // This should not happen
            free_entry(entry);
            return FIM_FILE_ERROR;
        }

        if (syscheck.opts[configuration] & CHECK_SEECHANGES) {
            fim_diff_process_delete_file(entry->file_entry.path); // LCOV_EXCL_LINE
        }

        if (fim_db_remove_path(syscheck.database, entry->file_entry.path) == FIMDB_ERR) {
            free_entry(entry);
            return FIM_FILE_ERROR;
        }

        *event = fim_json_event(entry->file_entry.path, NULL, entry->file_entry.data, configuration, FIM_DELETE,
                               FIM_SCHEDULED, NULL, NULL);
        free_entry(entry);

        return FIM_FILE_DELETED;
    }

    if (entry->file_entry.data->dev == item.statbuf.st_dev && entry->file_entry.data->inode == item.statbuf.st_ino) {
        goto end;
    }

    // We need to check if the new inode is being used in the DB
    switch (fim_db_data_exists(syscheck.database, item.statbuf.st_ino, item.statbuf.st_dev)) {
    case FIMDB_ERR:
        free_entry(entry);
        return FIM_FILE_ERROR;
    case 0:
        goto end;
    default:
    case 1:
        break;
    }

    // The inode is currently being used, scan those files first
    if (fim_db_append_paths_from_inode(syscheck.database, item.statbuf.st_ino, item.statbuf.st_dev, stack, tree) == 0) {
        // We have somehow reached a point an infinite loop could happen, we will need to update the current file
        // forcefully which will generate a false positive alert
        item.mode = FIM_SCHEDULED;
        item.index = fim_configuration_directory(entry->file_entry.path);
        if (item.index == -1) {
            // This should not happen
            free_entry(entry);     // LCOV_EXCL_LINE
            return FIM_FILE_ERROR; // LCOV_EXCL_LINE
        }
        item.configuration = syscheck.opts[item.index];

        *event = _fim_file_force_update(path, &item, entry);
        free_entry(entry);
        return FIM_FILE_UPDATED;
    }

    free_entry(entry);
    return FIM_FILE_ADDED_PATHS;

end:
    // Once here, either the used row was cleared and is available or this file is a hardlink to other file
    // either way the only thing left to do is to process the file
    item.mode = FIM_SCHEDULED;
    item.index = fim_configuration_directory(entry->file_entry.path);
    if (item.index == -1) {
        // This should not happen
        free_entry(entry);     // LCOV_EXCL_LINE
        return FIM_FILE_ERROR; // LCOV_EXCL_LINE
    }
    item.configuration = syscheck.opts[item.index];

    *event = _fim_file(entry->file_entry.path, &item, NULL);

    free_entry(entry);
    return FIM_FILE_UPDATED;
}

/**
 * @brief Resolves a conflict on the given inode.
 *
 * @param inode The inode that caused a collision with an existing DB entry.
 * @param dev The device that caused a collision with an existing DB entry.
 * @return 0 if the collision was solved correctly, -1 if an error occurred.
 */
static int fim_resolve_db_collision(unsigned long inode, unsigned long dev) {
    rb_tree *tree;
    OSList *stack;

    tree = rbtree_init();
    if (tree == NULL) {
        return -1; // LCOV_EXCL_LINE
    }

    stack = OSList_Create();
    if (stack == NULL) {
        rbtree_destroy(tree); // LCOV_EXCL_LINE
        return -1;            // LCOV_EXCL_LINE
    }

    fim_db_append_paths_from_inode(syscheck.database, inode, dev, stack, tree);
    w_mutex_unlock(&syscheck.fim_entry_mutex);

    while (stack->currently_size != 0) {
        char *current_path;
        cJSON *event = NULL;
        OSListNode *last = OSList_GetLastNode(stack);

        if (last == NULL) {
            mdebug2("Failed getting the next node to scan"); // LCOV_EXCL_LINE
            break;                                           // LCOV_EXCL_LINE
        }

        current_path = (char *)last->data;

        w_mutex_lock(&syscheck.fim_entry_mutex);

        switch (fim_process_file_from_db(current_path, stack, tree, &event)) {
        case FIM_FILE_UPDATED:
        case FIM_FILE_DELETED:
            OSList_DeleteCurrentlyNode(stack);
            break;
        case FIM_FILE_ADDED_PATHS:
            // Nothing to do here, we will move to the new last path and retry there
            break;
        case FIM_FILE_ERROR:
        default:
            OSList_Destroy(stack);
            rbtree_destroy(tree);
            return -1;
        }

        w_mutex_unlock(&syscheck.fim_entry_mutex);

        if (event) {
            send_syscheck_msg(event); // LCOV_EXCL_LINE
        }

        cJSON_Delete(event);
        event = NULL;
    }

    OSList_Destroy(stack);
    rbtree_destroy(tree);

    w_mutex_lock(&syscheck.fim_entry_mutex);

    return 0;
}
#endif

/**
 * @brief Makes any necessary queries to get the entry updated in the DB.
 *
 * @param path The path to the file being processed.
 * @param data The information linked to the path to be updated
 * @param saved If the file had information stored in the DB, that data is returned in this parameter.
 * @param event_mode The mode that triggered the event being processed.
 * @return The result of the update operation.
 * @retval FIMDB_ERR if an error occurs in the DB.
 * @retval -1 if an error occurs.
 * @retval 0 if the operation ends correctly.
 */
static int fim_update_db_data(const char *path,
                              const fim_file_data *data,
                              fim_entry **saved,
                              __attribute__((unused)) fim_event_mode event_mode) {
    assert(saved != NULL);

    *saved = fim_db_get_path(syscheck.database, path);

#ifndef WIN32
    // We will rely on realtime and whodata modes not losing deletion and creation events.
    // This will potentially trigger false positives in very particular cases and environments but
    // there is no easy way to implement the DB correction algorithm in those modes.
    if (event_mode != FIM_SCHEDULED) {
        return fim_db_insert(syscheck.database, path, data, *saved != NULL ? (*saved)->file_entry.data : NULL);
    }
#endif

    if (*saved == NULL) {
#ifndef WIN32
        switch (fim_db_data_exists(syscheck.database, data->inode, data->dev)) {
        case FIMDB_ERR:
            return -1;
        case 1:
            if (fim_resolve_db_collision(data->inode, data->dev) != 0) {
                mwarn("Failed to resolve an inode collision for file '%s'", path);
                return -1;
            }
            // Fallthrough
        case 0:
        default:
            return fim_db_insert(syscheck.database, path, data, NULL);
        }
#else // WIN32
        return fim_db_insert(syscheck.database, path, data, NULL);
#endif
    }

    if (strcmp(data->checksum, (*saved)->file_entry.data->checksum) == 0) {
        // Entry up to date
        fim_db_set_scanned(syscheck.database, path);
        return 0;
    }

#ifndef WIN32
    if (data->dev == (*saved)->file_entry.data->dev && data->inode == (*saved)->file_entry.data->inode) {
        return fim_db_insert(syscheck.database, path, data, (*saved)->file_entry.data);
    }

    switch (fim_db_data_exists(syscheck.database, data->inode, data->dev)) {
    case FIMDB_ERR:
        return -1;
    case 0:
        return fim_db_insert(syscheck.database, path, data, (*saved)->file_entry.data);
    case 1:
    default:
        break;
    }

    if (fim_resolve_db_collision(data->inode, data->dev) != 0) {
        mwarn("Failed to resolve an inode collision for file '%s'", path); // LCOV_EXCL_LINE
        return -1;                                                         // LCOV_EXCL_LINE
    }
#endif

    // At this point, we should be safe to store the new data
    return fim_db_insert(syscheck.database, path, data, (*saved)->file_entry.data);
}

/**
 * @brief Processes a file, update the DB entry and return an event. No mutex is used inside this function.
 *
 * @param path The path to the file being processed.
 * @param item Miscellaneous information linked to the event being processed.
 * @param w_evt Whodata information associated with the event.
 */
static cJSON *_fim_file(const char *path, fim_element *item, whodata_evt *w_evt) {
    fim_entry *saved = NULL;
    fim_file_data *new = NULL;
    cJSON *json_event = NULL;
    char *diff = NULL;
    int alert_type;

    if (item->mode == FIM_SCHEDULED) {
        // Prevent analysis of the same file twice during the same scan
        switch (fim_db_file_is_scanned(syscheck.database, path)) {
            case FIMDB_ERR:
                mdebug2("Failed to query status of file '%s'", path);
                // Fallthrough
            case 1:
                return NULL;
            case 0:
            default:
                break;
        }
    }

    //Get file attributes
    new = fim_get_data(path, item);
    if (new == NULL) {
        mdebug1(FIM_GET_ATTRIBUTES, path);
        return NULL;
    }

    if (fim_update_db_data(path, new, &saved, item->mode) != 0) {
        free_file_data(new);
        free_entry(saved);
        return NULL;
    }

    if (!saved) {
        alert_type = FIM_ADD; // New entry
    } else {
        alert_type = FIM_MODIFICATION; // Checking for changes
    }

    if (item->configuration & CHECK_SEECHANGES) {
        diff = fim_file_diff(path);
    }

    json_event = fim_json_event(path, saved ? saved->file_entry.data : NULL, new, item->index, alert_type, item->mode, w_evt, diff);

    os_free(diff);
    free_file_data(new);
    free_entry(saved);

    return json_event;
}

#ifndef WIN32
/**
 * @brief Virtually identical to `_fim_file`, except this function updates the DB with no further validations
 *
 * @param path The path to the file being processed.
 * @param item Miscellaneous information linked to the event being processed.
 * @param saved The data linked to the file that was extracted from the DB in a previous operation.
 */
static cJSON *_fim_file_force_update(const char *path, const fim_element *item, const fim_entry *saved) {
    fim_file_data *new = NULL;
    cJSON *json_event = NULL;
    char *diff = NULL;
    int alert_type;

    assert(path != NULL);
    assert(item != NULL);
    assert(saved != NULL);

    // Get file attributes
    new = fim_get_data(path, item);
    if (new == NULL) {
        mdebug1(FIM_GET_ATTRIBUTES, path);
        return NULL;
    }

    if (fim_db_insert(syscheck.database, path, new, saved->file_entry.data) != 0) {
        free_file_data(new);
        return NULL;
    }

    alert_type = FIM_MODIFICATION; // Checking for changes

    if (item->configuration & CHECK_SEECHANGES) {
        diff = fim_file_diff(path);
    }

    json_event = fim_json_event(path, saved->file_entry.data, new, item->index, alert_type, item->mode, NULL, diff);

    os_free(diff);
    free_file_data(new);

    return json_event;
}
#endif

void fim_file(const char *file, fim_element *item, whodata_evt *w_evt, int report) {
    cJSON *json_event = NULL;

    w_mutex_lock(&syscheck.fim_entry_mutex);
    json_event = _fim_file(file, item, w_evt);
    w_mutex_unlock(&syscheck.fim_entry_mutex);

    if (json_event && _base_line && report) {
        send_syscheck_msg(json_event);
    }

    cJSON_Delete(json_event);
}


void fim_realtime_event(char *file) {

    struct stat file_stat;

    // If the file exists, generate add or modify events.
    if (w_stat(file, &file_stat) >= 0) {
        /* Need a sleep here to avoid triggering on vim
         * (and finding the file removed)
         */
        fim_rt_delay();

        fim_element item = { .mode = FIM_REALTIME };
        fim_checker(file, &item, NULL, 1);
    }
    else {
        // Otherwise, it could be a file deleted or a directory moved (or renamed).
        fim_process_missing_entry(file, FIM_REALTIME, NULL);
    }
}

void fim_whodata_event(whodata_evt * w_evt) {

    struct stat file_stat;

    // If the file exists, generate add or modify events.
    if(w_stat(w_evt->path, &file_stat) >= 0) {
        fim_rt_delay();

        fim_element item = { .mode = FIM_WHODATA };
        fim_checker(w_evt->path, &item, w_evt, 1);
    }
    // Otherwise, it could be a file deleted or a directory moved (or renamed).
    else {
            fim_process_missing_entry(w_evt->path, FIM_WHODATA, w_evt);
        #ifndef WIN32
            char** paths = NULL;
            const unsigned long int inode = strtoul(w_evt->inode,NULL,10);
            const unsigned long int dev = strtoul(w_evt->dev,NULL,10);

            w_mutex_lock(&syscheck.fim_entry_mutex);
            paths = fim_db_get_paths_from_inode(syscheck.database, inode, dev);
            w_mutex_unlock(&syscheck.fim_entry_mutex);

            if(paths) {
                for(int i = 0; paths[i]; i++) {
                    fim_process_missing_entry(paths[i], FIM_WHODATA, w_evt);
                    os_free(paths[i]);
                }
                os_free(paths);
            }
        #endif
    }
}


void fim_process_missing_entry(char * pathname, fim_event_mode mode, whodata_evt * w_evt) {
    fim_entry *saved_data = NULL;
    fim_tmp_file *files = NULL;

    // Search path in DB.
    w_mutex_lock(&syscheck.fim_entry_mutex);
    saved_data = fim_db_get_path(syscheck.database, pathname);
    w_mutex_unlock(&syscheck.fim_entry_mutex);

    // Exists, create event.
    if (saved_data) {
        fim_element item = { .mode = mode };
        fim_checker(pathname, &item, w_evt, 1);
        free_entry(saved_data);
        return;
    }

    // Since the file doesn't exist, research if it's directory and have files in DB.
    char pattern[PATH_MAX] = {0};

    // Create the sqlite LIKE pattern -> "pathname/%"
    snprintf(pattern, PATH_MAX, "%s%c%%", pathname, PATH_SEP);

    w_mutex_lock(&syscheck.fim_entry_mutex);
    fim_db_get_path_from_pattern(syscheck.database, pattern, &files, syscheck.database_store);
    w_mutex_unlock(&syscheck.fim_entry_mutex);

    if (files && files->elements) {
        if (fim_db_process_missing_entry(syscheck.database, files, &syscheck.fim_entry_mutex,
            syscheck.database_store, mode, w_evt) != FIMDB_OK) {
                merror(FIM_DB_ERROR_RM_PATTERN, pattern);
        }
    }
}

// Checks the DB state, sends a message alert if necessary
void fim_check_db_state() {
    int nodes_count = 0;
    cJSON *json_event = NULL;
    char *json_plain = NULL;
    char alert_msg[OS_SIZE_256] = {'\0'};

    w_mutex_lock(&syscheck.fim_entry_mutex);
    nodes_count = fim_db_get_count_entries(syscheck.database);
    w_mutex_unlock(&syscheck.fim_entry_mutex);

    if (nodes_count < 0) {
        mwarn(FIM_DATABASE_NODES_COUNT_FAIL);
        return;
    }

    switch (_db_state) {
    case FIM_STATE_DB_FULL:
        if (nodes_count >= syscheck.file_limit) {
            return;
        }
        break;
    case FIM_STATE_DB_90_PERCENTAGE:
        if ((nodes_count < syscheck.file_limit) && (nodes_count >= syscheck.file_limit * 0.9)) {
            return;
        }
        break;
    case FIM_STATE_DB_80_PERCENTAGE:
        if ((nodes_count < syscheck.file_limit * 0.9) && (nodes_count >= syscheck.file_limit * 0.8)) {
            return;
        }
        break;
    case FIM_STATE_DB_NORMAL:
        if (nodes_count == 0) {
            _db_state = FIM_STATE_DB_EMPTY;
            return;
        }
        else if (nodes_count < syscheck.file_limit * 0.8) {
            return;
        }
        break;
    case FIM_STATE_DB_EMPTY:
        if (nodes_count == 0) {
            return;
        }
        else if (nodes_count < syscheck.file_limit * 0.8) {
            _db_state = FIM_STATE_DB_NORMAL;
            return;
        }
        break;
    default: // LCOV_EXCL_LINE
        break; // LCOV_EXCL_LINE
    }

    json_event = cJSON_CreateObject();
    cJSON_AddNumberToObject(json_event, "file_limit", syscheck.file_limit);
    cJSON_AddNumberToObject(json_event, "file_count", nodes_count);

    if (nodes_count >= syscheck.file_limit) {
        _db_state = FIM_STATE_DB_FULL;
        mwarn(FIM_DB_FULL_ALERT);
        cJSON_AddStringToObject(json_event, "alert_type", "full");
    }
    else if (nodes_count >= syscheck.file_limit * 0.9) {
        _db_state = FIM_STATE_DB_90_PERCENTAGE;
        minfo(FIM_DB_90_PERCENTAGE_ALERT);
        cJSON_AddStringToObject(json_event, "alert_type", "90_percentage");
    }
    else if (nodes_count >= syscheck.file_limit * 0.8) {
        _db_state = FIM_STATE_DB_80_PERCENTAGE;
        minfo(FIM_DB_80_PERCENTAGE_ALERT);
        cJSON_AddStringToObject(json_event, "alert_type", "80_percentage");
    }
    else if (nodes_count > 0) {
        _db_state = FIM_STATE_DB_NORMAL;
        minfo(FIM_DB_NORMAL_ALERT);
        cJSON_AddStringToObject(json_event, "alert_type", "normal");
    }
    else {
        _db_state = FIM_STATE_DB_EMPTY;
        minfo(FIM_DB_NORMAL_ALERT);
        cJSON_AddStringToObject(json_event, "alert_type", "normal");
    }

    json_plain = cJSON_PrintUnformatted(json_event);

    snprintf(alert_msg, OS_SIZE_256, "wazuh: FIM DB: %s", json_plain);

    send_log_msg(alert_msg);

    os_free(json_plain);
    cJSON_Delete(json_event);
}

// Returns the position of the path into directories array
int fim_configuration_directory(const char *path) {
    char full_path[OS_SIZE_4096 + 1] = {'\0'};
    char full_entry[OS_SIZE_4096 + 1] = {'\0'};
    char *real_path = NULL;
    int it = 0;
    int top = 0;
    int match = 0;
    int position = -1;

    if (!path || *path == '\0') {
        return position;
    }

    trail_path_separator(full_path, path, sizeof(full_path));

    while(syscheck.dir[it]) {
        real_path = fim_get_real_path(it);
        trail_path_separator(full_entry, real_path, sizeof(full_entry));
        match = w_compare_str(full_entry, full_path);

        free(real_path);
        if (top < match && full_path[match - 1] == PATH_SEP) {
            position = it;
            top = match;
        }
        it++;
    }

    if (position == -1) {
        mdebug2(FIM_CONFIGURATION_NOTFOUND, "file", path);
    }

    return position;
}

int fim_check_depth(const char * path, int dir_position) {
    const char * pos;
    int depth = -1;
    unsigned int parent_path_size;
    char *real_path = NULL;

    if (syscheck.dir[dir_position] == NULL && syscheck.symbolic_links[dir_position] == NULL) {
        return -1;
    }

    real_path = fim_get_real_path(dir_position);
    parent_path_size = strlen(real_path);
    free(real_path);

    if (parent_path_size > strlen(path)) {
        return -1;
    }

#ifdef WIN32
    // Check for monitoring of 'U:\'
    if(parent_path_size == 3 && path[2] == '\\') {
        depth = 0;
    }
#else
    // Check for monitoring of '/'
    if(parent_path_size == 1) {
        depth = 0;
    }
#endif

    pos = path + parent_path_size;
    while (pos) {
        if (pos = strchr(pos, PATH_SEP), pos) {
            depth++;
        } else {
            break;
        }
        pos++;
    }

    return depth;
}


// Get data from file
fim_file_data *fim_get_data(const char *file, const fim_element *item) {
    fim_file_data *data = NULL;

    os_calloc(1, sizeof(fim_file_data), data);
    init_fim_data_entry(data);

    if (item->configuration & CHECK_SIZE) {
        data->size = item->statbuf.st_size;
    }

    if (item->configuration & CHECK_PERM) {
#ifdef WIN32
        int error;
        char perm[OS_SIZE_6144 + 1];

        if (error = w_get_file_permissions(file, perm, OS_SIZE_6144), error) {
            mdebug1(FIM_EXTRACT_PERM_FAIL, file, error);
            free_file_data(data);
            return NULL;
        } else {
            data->perm = decode_win_permissions(perm);
        }
#else
        data->perm = agent_file_perm(item->statbuf.st_mode);
#endif
    }

#ifdef WIN32
    if (item->configuration & CHECK_ATTRS) {
        os_calloc(OS_SIZE_256, sizeof(char), data->attributes);
        decode_win_attributes(data->attributes, w_get_file_attrs(file));
    }
#endif

    if (item->configuration & CHECK_MTIME) {
#ifdef WIN32
        data->mtime = get_UTC_modification_time(file);
#else
        data->mtime = item->statbuf.st_mtime;
#endif
    }

#ifdef WIN32
    if (item->configuration & CHECK_OWNER) {
        data->user_name = get_file_user(file, &data->uid);
    }
#else
    if (item->configuration & CHECK_OWNER) {
        char aux[OS_SIZE_64];
        snprintf(aux, OS_SIZE_64, "%u", item->statbuf.st_uid);
        os_strdup(aux, data->uid);

        data->user_name = get_user(item->statbuf.st_uid);
    }

    if (item->configuration & CHECK_GROUP) {
        char aux[OS_SIZE_64];
        snprintf(aux, OS_SIZE_64, "%u", item->statbuf.st_gid);
        os_strdup(aux, data->gid);

        data->group_name = get_group(item->statbuf.st_gid);
    }
#endif

    snprintf(data->hash_md5, sizeof(os_md5), "%s", "d41d8cd98f00b204e9800998ecf8427e");
    snprintf(data->hash_sha1, sizeof(os_sha1), "%s", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    snprintf(data->hash_sha256, sizeof(os_sha256), "%s", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // The file exists and we don't have to delete it from the hash tables
    data->scanned = 1;

    // We won't calculate hash for symbolic links, empty or large files
    if ((item->statbuf.st_mode & S_IFMT) == FIM_REGULAR)
        if (item->statbuf.st_size > 0 &&
                (size_t)item->statbuf.st_size < syscheck.file_max_size &&
                ( item->configuration & CHECK_MD5SUM ||
                item->configuration & CHECK_SHA1SUM ||
                item->configuration & CHECK_SHA256SUM ) ) {
            if (OS_MD5_SHA1_SHA256_File(file,
                                        syscheck.prefilter_cmd,
                                        data->hash_md5,
                                        data->hash_sha1,
                                        data->hash_sha256,
                                        OS_BINARY,
                                        syscheck.file_max_size) < 0) {
                mdebug1(FIM_HASHES_FAIL, file);
                free_file_data(data);
                return NULL;
        }
    }

    if (!(item->configuration & CHECK_MD5SUM)) {
        data->hash_md5[0] = '\0';
    }

    if (!(item->configuration & CHECK_SHA1SUM)) {
        data->hash_sha1[0] = '\0';
    }

    if (!(item->configuration & CHECK_SHA256SUM)) {
        data->hash_sha256[0] = '\0';
    }

    data->inode = item->statbuf.st_ino;
    data->dev = item->statbuf.st_dev;
    data->mode = item->mode;
    data->options = item->configuration;
    data->last_event = time(NULL);
    fim_get_checksum(data);

    return data;
}

void init_fim_data_entry(fim_file_data *data) {
    data->size = 0;
    data->perm = NULL;
    data->attributes = NULL;
    data->uid = NULL;
    data->gid = NULL;
    data->user_name = NULL;
    data->group_name = NULL;
    data->mtime = 0;
    data->inode = 0;
    data->hash_md5[0] = '\0';
    data->hash_sha1[0] = '\0';
    data->hash_sha256[0] = '\0';
}

void fim_get_checksum (fim_file_data * data) {
    char *checksum = NULL;
    int size;

    size = snprintf(0,
            0,
            "%d:%s:%s:%s:%s:%s:%s:%u:%lu:%s:%s:%s",
            data->size,
            data->perm ? data->perm : "",
            data->attributes ? data->attributes : "",
            data->uid ? data->uid : "",
            data->gid ? data->gid : "",
            data->user_name ? data->user_name : "",
            data->group_name ? data->group_name : "",
            data->mtime,
            data->inode,
            data->hash_md5,
            data->hash_sha1,
            data->hash_sha256);

    os_calloc(size + 1, sizeof(char), checksum);
    snprintf(checksum,
            size + 1,
            "%d:%s:%s:%s:%s:%s:%s:%u:%lu:%s:%s:%s",
            data->size,
            data->perm ? data->perm : "",
            data->attributes ? data->attributes : "",
            data->uid ? data->uid : "",
            data->gid ? data->gid : "",
            data->user_name ? data->user_name : "",
            data->group_name ? data->group_name : "",
            data->mtime,
            data->inode,
            data->hash_md5,
            data->hash_sha1,
            data->hash_sha256);

    OS_SHA1_Str(checksum, -1, data->checksum);
    free(checksum);
}

void check_deleted_files() {
    fim_tmp_file *file = NULL;
    w_mutex_lock(&syscheck.fim_entry_mutex);

    if (fim_db_get_not_scanned(syscheck.database, &file, syscheck.database_store) != FIMDB_OK) {
        merror(FIM_DB_ERROR_RM_NOT_SCANNED);
    }

    w_mutex_unlock(&syscheck.fim_entry_mutex);

    if (file && file->elements) {
        fim_db_delete_not_scanned(syscheck.database, file, &syscheck.fim_entry_mutex, syscheck.database_store);
    }
}

cJSON *fim_json_event(const char *file_name,
                      fim_file_data *old_data,
                      fim_file_data *new_data,
                      int pos,
                      unsigned int type,
                      fim_event_mode mode,
                      const whodata_evt *w_evt,
                      const char *diff) {
    cJSON * changed_attributes = NULL;

    if (old_data != NULL) {
        changed_attributes = fim_json_compare_attrs(old_data, new_data);

        // If no such changes, do not send event.

        if (cJSON_GetArraySize(changed_attributes) == 0) {
            cJSON_Delete(changed_attributes);
            return NULL;
        }
    }

    cJSON * json_event = cJSON_CreateObject();
    cJSON_AddStringToObject(json_event, "type", "event");

    cJSON * data = cJSON_CreateObject();
    cJSON_AddItemToObject(json_event, "data", data);

    cJSON_AddStringToObject(data, "path", file_name);
    cJSON_AddNumberToObject(data, "version", 2.0);

    cJSON_AddStringToObject(data, "mode", FIM_EVENT_MODE[mode]);
    cJSON_AddStringToObject(data, "type", FIM_EVENT_TYPE[type]);
    cJSON_AddNumberToObject(data, "timestamp", new_data->last_event);

#ifndef WIN32
    char** paths = NULL;

    if (paths = fim_db_get_paths_from_inode(syscheck.database, new_data->inode, new_data->dev), paths){
        if (paths[0] && paths[1]) {
            cJSON *hard_links = cJSON_CreateArray();
            int i;
            for(i = 0; paths[i]; i++) {
                if(strcmp(file_name, paths[i])) {
                    cJSON_AddItemToArray(hard_links, cJSON_CreateString(paths[i]));
                }
                os_free(paths[i]);
            }
            cJSON_AddItemToObject(data, "hard_links", hard_links);
        } else {
            os_free(paths[0]);
        }
        os_free(paths);
    }
#endif

    cJSON_AddItemToObject(data, "attributes", fim_attributes_json(new_data));

    if (old_data) {
        cJSON_AddItemToObject(data, "changed_attributes", changed_attributes);
        cJSON_AddItemToObject(data, "old_attributes", fim_attributes_json(old_data));
    }

    char * tags = NULL;
    if (w_evt) {
        cJSON_AddItemToObject(data, "audit", fim_audit_json(w_evt));
    }

    tags = syscheck.tag[pos];

    if (diff != NULL) {
        cJSON_AddStringToObject(data, "content_changes", diff);
    }

    if (tags != NULL) {
        cJSON_AddStringToObject(data, "tags", tags);
    }

    return json_event;
}

// Create file attribute set JSON from a FIM entry structure

cJSON * fim_attributes_json(const fim_file_data * data) {
    cJSON * attributes = cJSON_CreateObject();

    // TODO: Read structure.
    // SQLite Development
    cJSON_AddStringToObject(attributes, "type", "file");

    if (data->options & CHECK_SIZE) {
        cJSON_AddNumberToObject(attributes, "size", data->size);
    }

    if (data->options & CHECK_PERM) {
        cJSON_AddStringToObject(attributes, "perm", data->perm);
    }

    if (data->options & CHECK_OWNER) {
        cJSON_AddStringToObject(attributes, "uid", data->uid);
    }

    if (data->options & CHECK_GROUP) {
        cJSON_AddStringToObject(attributes, "gid", data->gid);
    }

    if (data->user_name) {
        cJSON_AddStringToObject(attributes, "user_name", data->user_name);
    }

    if (data->group_name) {
        cJSON_AddStringToObject(attributes, "group_name", data->group_name);
    }

    if (data->options & CHECK_INODE) {
        cJSON_AddNumberToObject(attributes, "inode", data->inode);
    }

    if (data->options & CHECK_MTIME) {
        cJSON_AddNumberToObject(attributes, "mtime", data->mtime);
    }

    if (data->options & CHECK_MD5SUM) {
        cJSON_AddStringToObject(attributes, "hash_md5", data->hash_md5);
    }

    if (data->options & CHECK_SHA1SUM) {
        cJSON_AddStringToObject(attributes, "hash_sha1", data->hash_sha1);
    }

    if (data->options & CHECK_SHA256SUM) {
        cJSON_AddStringToObject(attributes, "hash_sha256", data->hash_sha256);
    }

#ifdef WIN32
    if (data->options & CHECK_ATTRS) {
        cJSON_AddStringToObject(attributes, "attributes", data->attributes);
    }
#endif

    if (*data->checksum) {
        cJSON_AddStringToObject(attributes, "checksum", data->checksum);
    }

    return attributes;
}

// Create file attribute comparison JSON object

cJSON * fim_json_compare_attrs(const fim_file_data * old_data, const fim_file_data * new_data) {
    cJSON * changed_attributes = cJSON_CreateArray();

    if ( (old_data->options & CHECK_SIZE) && (old_data->size != new_data->size) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("size"));
    }

    if ( (old_data->options & CHECK_PERM) && strcmp(old_data->perm, new_data->perm) != 0 ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("permission"));
    }

#ifdef WIN32
    if ( (old_data->options & CHECK_ATTRS) && strcmp(old_data->attributes, new_data->attributes) != 0 ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("attributes"));
    }
#endif

    if (old_data->options & CHECK_OWNER) {
        if (old_data->uid && new_data->uid && strcmp(old_data->uid, new_data->uid) != 0) {
            cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("uid"));
        }

        if (old_data->user_name && new_data->user_name && strcmp(old_data->user_name, new_data->user_name) != 0) {
            cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("user_name"));
        }
    }

    if (old_data->options & CHECK_GROUP) {
        if (old_data->gid && new_data->gid && strcmp(old_data->gid, new_data->gid) != 0) {
            cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("gid"));
        }

        if (old_data->group_name && new_data->group_name && strcmp(old_data->group_name, new_data->group_name) != 0) {
            cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("group_name"));
        }
    }

    if ( (old_data->options & CHECK_MTIME) && (old_data->mtime != new_data->mtime) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("mtime"));
    }

#ifndef WIN32
    if ( (old_data->options & CHECK_INODE) && (old_data->inode != new_data->inode) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("inode"));
    }
#endif

    if ( (old_data->options & CHECK_MD5SUM) && (strcmp(old_data->hash_md5, new_data->hash_md5) != 0) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("md5"));
    }

    if ( (old_data->options & CHECK_SHA1SUM) && (strcmp(old_data->hash_sha1, new_data->hash_sha1) != 0) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("sha1"));
    }

    if ( (old_data->options & CHECK_SHA256SUM) && (strcmp(old_data->hash_sha256, new_data->hash_sha256) != 0) ) {
        cJSON_AddItemToArray(changed_attributes, cJSON_CreateString("sha256"));
    }

    return changed_attributes;
}

// Create file audit data JSON object

cJSON * fim_audit_json(const whodata_evt * w_evt) {
    cJSON * fim_audit = cJSON_CreateObject();

    cJSON_AddStringToObject(fim_audit, "user_id", w_evt->user_id);
    cJSON_AddStringToObject(fim_audit, "user_name", w_evt->user_name);
    cJSON_AddStringToObject(fim_audit, "process_name", w_evt->process_name);
    cJSON_AddNumberToObject(fim_audit, "process_id", w_evt->process_id);
#ifndef WIN32
    cJSON_AddStringToObject(fim_audit, "cwd", w_evt->cwd);
    cJSON_AddStringToObject(fim_audit, "group_id", w_evt->group_id);
    cJSON_AddStringToObject(fim_audit, "group_name", w_evt->group_name);
    cJSON_AddStringToObject(fim_audit, "audit_uid", w_evt->audit_uid);
    cJSON_AddStringToObject(fim_audit, "audit_name", w_evt->audit_name);
    cJSON_AddStringToObject(fim_audit, "effective_uid", w_evt->effective_uid);
    cJSON_AddStringToObject(fim_audit, "effective_name", w_evt->effective_name);
    cJSON_AddStringToObject(fim_audit, "parent_name", w_evt->parent_name);
    cJSON_AddStringToObject(fim_audit, "parent_cwd", w_evt->parent_cwd);
    cJSON_AddNumberToObject(fim_audit, "ppid", w_evt->ppid);
#endif

    return fim_audit;
}


// Create scan info JSON event

cJSON * fim_scan_info_json(fim_scan_event event, long timestamp) {
    cJSON * root = cJSON_CreateObject();
    cJSON * data = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "type", event == FIM_SCAN_START ? "scan_start" : "scan_end");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddNumberToObject(data, "timestamp", timestamp);

    return root;
}

int fim_check_ignore (const char *file_name) {
    // Check if the file should be ignored
    if (syscheck.ignore) {
        int i = 0;
        while (syscheck.ignore[i] != NULL) {
            if (strncasecmp(syscheck.ignore[i], file_name, strlen(syscheck.ignore[i])) == 0) {
                mdebug2(FIM_IGNORE_ENTRY, "file", file_name, syscheck.ignore[i]);
                return 1;
            }
            i++;
        }
    }

    // Check in the regex entry
    if (syscheck.ignore_regex) {
        int i = 0;
        while (syscheck.ignore_regex[i] != NULL) {
            if (OSMatch_Execute(file_name, strlen(file_name), syscheck.ignore_regex[i])) {
                mdebug2(FIM_IGNORE_SREGEX, "file", file_name, syscheck.ignore_regex[i]->raw);
                return 1;
            }
            i++;
        }
    }

    return 0;
}


int fim_check_restrict (const char *file_name, OSMatch *restriction) {
    if (file_name == NULL) {
        merror(NULL_ERROR);
        return 1;
    }

    // Restrict file types
    if (restriction) {
        if (!OSMatch_Execute(file_name, strlen(file_name), restriction)) {
            mdebug2(FIM_FILE_IGNORE_RESTRICT, file_name, restriction->raw);
            return 1;
        }
    }

    return 0;
}


void free_file_data(fim_file_data * data) {
    if (!data) {
        return;
    }

    os_free(data->perm);
    os_free(data->attributes);
    os_free(data->uid);
    os_free(data->gid);
    os_free(data->user_name);
    os_free(data->group_name);

    os_free(data);
}


void free_entry(fim_entry * entry) {
    if (entry) {
#ifndef WIN32
        os_free(entry->file_entry.path);
        free_file_data(entry->file_entry.data);
        free(entry);
#else
        if (entry->type == FIM_TYPE_FILE) {
            os_free(entry->file_entry.path);
            free_file_data(entry->file_entry.data);
            free(entry);
        } else {
            fim_registry_free_entry(entry);
        }
#endif
    }
}


void fim_diff_folder_size(){
    char *diff_local;

    os_malloc(strlen(DIFF_DIR) + strlen("/local") + 1, diff_local);

    snprintf(diff_local, strlen(DIFF_DIR) + strlen("/local") + 1, "%s/local", DIFF_DIR);

    if (IsDir(diff_local) == 0) {
        syscheck.diff_folder_size = DirSize(diff_local) / 1024;
    }

    os_free(diff_local);
}

// LCOV_EXCL_START
void fim_print_info(struct timespec start, struct timespec end, clock_t cputime_start) {
    mdebug1(FIM_RUNNING_SCAN,
            time_diff(&start, &end),
            (double)(clock() - cputime_start) / CLOCKS_PER_SEC);

#ifdef WIN32
    mdebug1(FIM_ENTRIES_INFO, fim_db_get_count_file_entry(syscheck.database));
    mdebug1(FIM_REGISTRY_ENTRIES_INFO, fim_db_get_count_registry_key(syscheck.database) + fim_db_get_count_registry_data(syscheck.database));
#else
    unsigned inode_items = 0;
    unsigned inode_paths = 0;

    inode_items = fim_db_get_count_file_data(syscheck.database);
    inode_paths = fim_db_get_count_file_entry(syscheck.database);

    mdebug1(FIM_INODES_INFO, inode_items, inode_paths);
#endif

    return;
}

char *fim_get_real_path(int position) {
    char *real_path = NULL;

#ifndef WIN32
    w_mutex_lock(&syscheck.fim_symlink_mutex);

    //Create a safe copy of the path to be used by other threads.
    if ((syscheck.opts[position] & CHECK_FOLLOW) == 0) {
        os_strdup(syscheck.dir[position], real_path);
    } else if (syscheck.symbolic_links[position]) {
        os_strdup(syscheck.symbolic_links[position], real_path);
    } else if (IsLink(syscheck.dir[position]) == 0) { // Broken link
        os_strdup("", real_path);
    } else {
        os_strdup(syscheck.dir[position], real_path);
    }

    w_mutex_unlock(&syscheck.fim_symlink_mutex);
#else // WIN32
    os_strdup(syscheck.dir[position], real_path);
#endif

    return real_path;
}

// Sleep during rt_delay milliseconds

void fim_rt_delay() {
    if (syscheck.rt_delay){
#ifdef WIN32
        Sleep(syscheck.rt_delay);
#else
        struct timeval timeout = {0, syscheck.rt_delay * 1000};
        select(0, NULL, NULL, NULL, &timeout);
#endif
    }
}

// LCOV_EXCL_STOP
