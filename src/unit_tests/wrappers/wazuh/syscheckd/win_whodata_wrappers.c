/* Copyright (C) 2015-2021, Wazuh Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#include "win_whodata_wrappers.h"
#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>

int __wrap_run_whodata_scan() {
    return mock();
}

int __wrap_set_winsacl(const char *dir, int position) {
    check_expected(dir);
    check_expected(position);

    return mock();
}

int __wrap_whodata_audit_start() {
    return 0;
}
