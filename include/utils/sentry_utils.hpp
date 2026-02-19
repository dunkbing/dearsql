#pragma once

#include <sentry.h>
#include <string>

namespace SentryUtils {

    inline void addBreadcrumb(const char* category, const char* message,
                              const char* level = "info") {
        sentry_value_t crumb = sentry_value_new_breadcrumb("default", message);
        sentry_value_set_by_key(crumb, "category", sentry_value_new_string(category));
        sentry_value_set_by_key(crumb, "level", sentry_value_new_string(level));
        sentry_add_breadcrumb(crumb);
    }

    inline void addBreadcrumb(const char* category, const char* message, const char* key,
                              const std::string& value, const char* level = "info") {
        sentry_value_t crumb = sentry_value_new_breadcrumb("default", message);
        sentry_value_set_by_key(crumb, "category", sentry_value_new_string(category));
        sentry_value_set_by_key(crumb, "level", sentry_value_new_string(level));
        sentry_value_t data = sentry_value_new_object();
        sentry_value_set_by_key(data, key, sentry_value_new_string(value.c_str()));
        sentry_value_set_by_key(crumb, "data", data);
        sentry_add_breadcrumb(crumb);
    }

} // namespace SentryUtils
