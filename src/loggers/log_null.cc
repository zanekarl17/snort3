/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
** Copyright (C) 2002-2013 Sourcefire, Inc.
** Copyright (C) 1998-2002 Martin Roesch <roesch@sourcefire.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/* This module is a NULL placeholder for people that want to turn off
 * logging for whatever reason.  Please note that logging is separate from
 * alerting, they are completely separate output facilities within Snort.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>

#include "framework/logger.h"
#include "framework/module.h"
#include "snort.h"

//-------------------------------------------------------------------------
// log_null module
//-------------------------------------------------------------------------

static const Parameter null_params[] =
{
    // FIXIT this definitely has no params
    // is module needed?  should this even be user facing?
    // the purpose is to override a conf from cmdline to stop logging

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

class NullModule : public Module
{
public:
    NullModule() : Module("log_null", null_params) { };
    bool set(const char*, Value&, SnortConfig*) { return false; };
};

class NullLogger : public Logger {
public:
    NullLogger() { };
};

static Module* mod_ctor()
{ return new NullModule; }

static void mod_dtor(Module* m)
{ delete m; }

static Logger* null_ctor(SnortConfig*, Module*)
{ return new NullLogger; }

static void null_dtor(Logger* p)
{ delete p; }

static LogApi null_api
{
    {
        PT_LOGGER,
        "log_null",
        LOGAPI_PLUGIN_V0,
        0,
        mod_ctor,
        mod_dtor
    },
    OUTPUT_TYPE_FLAG__LOG,
    null_ctor,
    null_dtor
};

#ifdef BUILDING_SO
SO_PUBLIC const BaseApi* snort_plugins[] =
{
    &null_api.base,
    nullptr
};
#else
const BaseApi* log_null = &null_api.base;
#endif
