/**
 * @section LICENCE
 *
 * This file is distributed as part of the MariaDB Corporation MaxScale. It is
 * free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 *
 * Copyright MariaDB Corporation Ab
 *
 * @file
 *
 */

#include <sqliteInt.h>
#include <log_manager.h>
#include <modinfo.h>
#include <mysql_client_server_protocol.h>
#include <platform.h>
#include <query_classifier.h>

//#define QC_TRACE_ENABLED
#undef QC_TRACE_ENABLED

#if defined(QC_TRACE_ENABLED)
#define QC_TRACE() MXS_NOTICE(__func__)
#else
#define QC_TRACE()
#endif

/**
 * The status of the information in QC_SQLITE_INFO.
 */
typedef enum qc_info_status
{
    QC_INFO_OK      = 0, // The information is valid.
    QC_INFO_INVALID = 1  // The information is invalid.
} qc_info_status_t;

/**
 * Contains information about a particular query.
 */
typedef struct qc_sqlite_info
{
    qc_info_status_t status;  // The validity of the information in this structure.
    // TODO: More to be added.
    qc_query_op_t operation;
} QC_SQLITE_INFO;

/**
 * The state of qc_sqlite.
 */
static struct
{
    bool initialized;
} this_unit;

/**
 * The qc_sqlite thread-specific state.
 */
static thread_local struct
{
    bool initialized;
    sqlite3* db;      // Thread specific database handle.
    QC_SQLITE_INFO* info;
} this_thread;


/**
 * HELPERS
 */

static void buffer_object_free(void* data);
static bool ensure_query_is_parsed(GWBUF* query);
static QC_SQLITE_INFO* get_query_info(GWBUF* query);
static QC_SQLITE_INFO* info_alloc(void);
static void info_finish(QC_SQLITE_INFO* info);
static void info_free(QC_SQLITE_INFO* info);
static QC_SQLITE_INFO* info_init(QC_SQLITE_INFO* info);
static bool parse_query(GWBUF* query);
static void parse_query_string(const char* query, size_t len);
static bool query_is_parsed(GWBUF* query);


/**
 * Used for freeing a QC_SQLITE_INFO object added to a GWBUF.
 *
 * @param object A pointer to a QC_SQLITE_INFO object.
 */
static void buffer_object_free(void* data)
{
    info_free((QC_SQLITE_INFO*) data);
}

static bool ensure_query_is_parsed(GWBUF* query)
{
    bool parsed = query_is_parsed(query);

    if (!parsed)
    {
        parsed = parse_query(query);
    }

    return parsed;
}

static QC_SQLITE_INFO* get_query_info(GWBUF* query)
{
    QC_SQLITE_INFO* info = NULL;

    if (ensure_query_is_parsed(query))
    {
        info = (QC_SQLITE_INFO*) gwbuf_get_buffer_object_data(query, GWBUF_PARSING_INFO);
        ss_dassert(info);
    }

    return info;
}

static QC_SQLITE_INFO* info_alloc(void)
{
    QC_SQLITE_INFO* info = malloc(sizeof(*info));

    if (info)
    {
        info_init(info);
    }

    return info;
}

static void info_finish(QC_SQLITE_INFO* info)
{
    (void) info;
}

static void info_free(QC_SQLITE_INFO* info)
{
    if (info)
    {
        info_finish(info);
        free(info);
    }
}

static QC_SQLITE_INFO* info_init(QC_SQLITE_INFO* info)
{
    memset(info, 0, sizeof(*info));

    info->status = QC_INFO_INVALID;
    info->operation = (qc_query_op_t) -1;

    return info;
}

static void parse_query_string(const char* query, size_t len)
{
    sqlite3_stmt* stmt = NULL;
    const char* tail = NULL;

    ss_dassert(this_thread.db);
    int rc = sqlite3_prepare(this_thread.db, query, len, &stmt, &tail);

    if (rc != SQLITE_OK)
    {
        MXS_DEBUG("qc_sqlite: sqlite3_prepare returned something else but ok: %d, %s",
                  rc, sqlite3_errstr(rc));
    }

    if (stmt)
    {
        sqlite3_finalize(stmt);
    }
}

static bool parse_query(GWBUF* query)
{
    bool parsed = false;
    ss_dassert(!query_is_parsed(query));

    QC_SQLITE_INFO* info = info_alloc();

    if (info)
    {
        this_thread.info = info;

        // TODO: Somewhere it needs to be ensured that this buffer is contiguous.
        // TODO: Where is it checked that the GWBUF really contains a query?
        uint8_t* data = (uint8_t*) GWBUF_DATA(query);
        size_t len = MYSQL_GET_PACKET_LEN(data) - 1; // Subtract 1 for packet type byte.

        const char* s = (const char*) &data[5]; // TODO: Are there symbolic constants somewhere?

        parse_query_string(s, len);

        if (this_thread.info->status == QC_INFO_OK)
        {
            MXS_INFO("qc_sqlite: SQL statement \"%.*s\", was recognized.", (int)len, s);
        }
        else
        {
            MXS_ERROR("qc_sqlite: SQL statement \"%.*s\", was not recognized.", (int)len, s);
        }

        // TODO: Add return value to gwbuf_add_buffer_object.
        // Always added; also when it was not recognized. If it was not recognized now,
        // it won't be if we try a second time.
        gwbuf_add_buffer_object(query, GWBUF_PARSING_INFO, info, buffer_object_free);
        parsed = true;

        this_thread.info = NULL;
    }
    else
    {
        MXS_ERROR("Could not allocate structure for containing parse data.");
    }

    return parsed;
}

static bool query_is_parsed(GWBUF* query)
{
    return query && GWBUF_IS_PARSED(query);
}


/**
 * SQLITE
 *
 * These functions are called from sqlite.
 */

int qc_sqlite3Select(Parse* pParse, Select* p, SelectDest* pDest)
{
    MXS_NOTICE("qc_sqlite: qc_sqlite3Select called.");

    QC_SQLITE_INFO* info = this_thread.info;
    ss_dassert(info);

    info->status = QC_INFO_OK;
    info->operation = QUERY_OP_SELECT;

    return -1;
}

/**
 * API
 */
static bool qc_sqlite_init(void);
static void qc_sqlite_end(void);
static bool qc_sqlite_thread_init(void);
static void qc_sqlite_thread_end(void);
static qc_query_type_t qc_sqlite_get_type(GWBUF* query);
static qc_query_op_t qc_sqlite_get_operation(GWBUF* query);
static char* qc_sqlite_get_created_table_name(GWBUF* query);
static bool qc_sqlite_is_drop_table_query(GWBUF* query);
static bool qc_sqlite_is_real_query(GWBUF* query);
static char** qc_sqlite_get_table_names(GWBUF* query, int* tblsize, bool fullnames);
static char* qc_sqlite_get_canonical(GWBUF* query);
static bool qc_sqlite_query_has_clause(GWBUF* query);
static char* qc_sqlite_get_qtype_str(qc_query_type_t qtype);
static char* qc_sqlite_get_affected_fields(GWBUF* query);
static char** qc_sqlite_get_database_names(GWBUF* query, int* sizep);

static bool qc_sqlite_init(void)
{
    QC_TRACE();
    assert(!this_unit.initialized);

    if (sqlite3_initialize() == 0)
    {
        this_unit.initialized = true;

        if (!qc_sqlite_thread_init())
        {
            this_unit.initialized = false;

            sqlite3_shutdown();
        }
    }
    else
    {
        MXS_ERROR("Failed to initialize sqlite3.");
    }

    return this_unit.initialized;
}

static void qc_sqlite_end(void)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);

    qc_sqlite_thread_end();

    sqlite3_shutdown();
    this_unit.initialized = false;
}

static bool qc_sqlite_thread_init(void)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(!this_thread.initialized);

    // TODO: It may be sufficient to have a single in-memory database for all threads.
    int rc = sqlite3_open(":memory:", &this_thread.db);
    if (rc == SQLITE_OK)
    {
        this_thread.initialized = true;

        MXS_INFO("In-memory sqlite database successfully opened for thread %lu.",
                 (unsigned long) pthread_self());
    }
    else
    {
        MXS_ERROR("Failed to open in-memory sqlite database for thread %lu: %d, %s",
                  (unsigned long) pthread_self(), rc, sqlite3_errstr(rc));
    }

    return this_thread.initialized;
}

static void qc_sqlite_thread_end(void)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    ss_dassert(this_thread.db);
    int rc = sqlite3_close(this_thread.db);

    if (rc != SQLITE_OK)
    {
        MXS_WARNING("The closing of the thread specific sqlite database failed: %d, %s",
                    rc, sqlite3_errstr(rc));
    }

    this_thread.db = NULL;
    this_thread.initialized = false;
}

static qc_query_type_t qc_sqlite_get_type(GWBUF* query)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    MXS_ERROR("qc_sqlite: qc_get_type not implemented yet.");

    return QUERY_TYPE_UNKNOWN;
}

static qc_query_op_t qc_sqlite_get_operation(GWBUF* query)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    qc_query_op_t op = QUERY_OP_UNDEFINED;
    QC_SQLITE_INFO* info = get_query_info(query);

    if (info)
    {
        if (info->status == QC_INFO_OK)
        {
            op = info->operation;
        }
        else
        {
            MXS_ERROR("qc_sqlite: The query operation was not resolved. Response not valid.");
        }
    }
    else
    {
        MXS_ERROR("qc_sqlite: The query could not be parsed. Response not valid.");
    }

    return op;
}

static char* qc_sqlite_get_created_table_name(GWBUF* query)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    MXS_ERROR("qc_sqlite: qc_get_created_table_name not implemented yet.");

    return NULL;
}

static bool qc_sqlite_is_drop_table_query(GWBUF* query)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    MXS_ERROR("qc_sqlite: qc_is_drop_table_query not implemented yet.");

    return false;
}

static bool qc_sqlite_is_real_query(GWBUF* query)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    MXS_ERROR("qc_sqlite: qc_is_real_query not implemented yet.");

    return false;
}

static char** qc_sqlite_get_table_names(GWBUF* query, int* tblsize, bool fullnames)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    MXS_ERROR("qc_sqlite: qc_get_table names not implemented yet.");

    return NULL;
}

static char* qc_sqlite_get_canonical(GWBUF* query)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    MXS_ERROR("qc_sqlite: qc_get_canonical not implemented yet.");

    return NULL;
}

static bool qc_sqlite_query_has_clause(GWBUF* query)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    MXS_ERROR("qc_sqlite: qc_query_has_clause not implemented yet.");

    return NULL;
}

static char* qc_sqlite_get_qtype_str(qc_query_type_t qtype)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    MXS_ERROR("qc_sqlite: qc_get_type_str not implemented yet.");

    return NULL;
}

static char* qc_sqlite_get_affected_fields(GWBUF* query)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    MXS_ERROR("qc_sqlite: qc_get_affected_files not implemented yet.");

    return NULL;
}

static char** qc_sqlite_get_database_names(GWBUF* query, int* sizep)
{
    QC_TRACE();
    ss_dassert(this_unit.initialized);
    ss_dassert(this_thread.initialized);

    MXS_ERROR("qc_sqlite: qc_get_database_names not implemented yet.");

    return NULL;
}

/**
 * EXPORTS
 */

static char version_string[] = "V1.0.0";

static QUERY_CLASSIFIER qc =
{
    qc_sqlite_init,
    qc_sqlite_end,
    qc_sqlite_thread_init,
    qc_sqlite_thread_end,
    qc_sqlite_get_type,
    qc_sqlite_get_operation,
    qc_sqlite_get_created_table_name,
    qc_sqlite_is_drop_table_query,
    qc_sqlite_is_real_query,
    qc_sqlite_get_table_names,
    qc_sqlite_get_canonical,
    qc_sqlite_query_has_clause,
    qc_sqlite_get_qtype_str,
    qc_sqlite_get_affected_fields,
    qc_sqlite_get_database_names,
};


MODULE_INFO info =
{
    MODULE_API_QUERY_CLASSIFIER,
    MODULE_IN_DEVELOPMENT,
    QUERY_CLASSIFIER_VERSION,
    "Query classifier using sqlite.",
};

char* version()
{
    return version_string;
}

void ModuleInit()
{
}

QUERY_CLASSIFIER* GetModuleObject()
{
    return &qc;
}