#ifndef BDTRACE_SCHEMA_H
#define BDTRACE_SCHEMA_H

namespace bdtrace {

const char* get_schema_sql();
const char* get_schema_v2_upgrade_sql();
const char* get_schema_v3_upgrade_sql();
const char* get_schema_v4_upgrade_sql();

} // namespace bdtrace

#endif // BDTRACE_SCHEMA_H
