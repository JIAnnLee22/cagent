/*
 * report/report.h — static self-development assessment data.
 *
 * This module deliberately has no TUI or terminal dependency. The data is a
 * compile-time snapshot sourced from TODO.md; update both documents together
 * when the assessment changes.
 */

#ifndef CAGENT_REPORT_REPORT_H
#define CAGENT_REPORT_REPORT_H

#include <stddef.h>

typedef struct {
    const char* label;
    int weight;
    int current;
} ReportDimension;

typedef struct {
    int priority; /* 0=P0, 1=P1, 2=P2, 3=P3 */
    const char* title;
    int completeness;
    const char* const* items;
    size_t items_len;
} ReportGap;

int report_overall_percent(void);
size_t report_dimension_count(void);
const ReportDimension* report_dimensions(void);
size_t report_gap_count(void);
const ReportGap* report_gaps(void);
size_t report_done_count(void);
const char* const* report_dones(void);
size_t report_roadmap_count(void);
const char* const* report_roadmap(void);
const char* report_title(void);
const char* report_conclusion(void);

#endif /* CAGENT_REPORT_REPORT_H */
