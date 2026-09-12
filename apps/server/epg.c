#include "epg.h"
#include "rs_common.h"
#include "rs_provider_options.h"
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void convert_time(xmlNode *node, const char *key, int offset) {
    xmlChar *value = xmlGetProp(node, (const xmlChar *)key);
    if (!value) return;
    const char *s = (const char *)value;
    size_t n = strlen(s);
    bool complete = n >= 14;
    for (size_t i = 0; i < 14 && complete; i++) complete = isdigit((unsigned char)s[i]) != 0;
    if (!complete) { xmlFree(value); return; } // partial XMLTV dates have no single instant
    int y, m, d, h, min, sec;
    if (sscanf(s, "%4d%2d%2d%2d%2d%2d", &y, &m, &d, &h, &min, &sec) != 6 ||
        y < 1970 || m < 1 || m > 12 || d < 1 || d > 31 || h > 23 || min > 59 || sec > 59) {
        xmlFree(value); return;
    }
    const char *zone = s + 14;
    while (*zone == ' ') zone++;
    int original = 0;
    if (*zone && !rs_provider_timezone_offset(zone, &original)) { xmlFree(value); return; }
    struct tm tm = {0};
    tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = min; tm.tm_sec = sec;
#ifdef _WIN32
    time_t epoch = _mkgmtime(&tm);
#else
    time_t epoch = timegm(&tm);
#endif
    epoch += (offset - original) * 60;
#ifdef _WIN32
    gmtime_s(&tm, &epoch);
#else
    gmtime_r(&epoch, &tm);
#endif
    char result[48], stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm);
    int magnitude = offset < 0 ? -offset : offset;
    snprintf(result, sizeof(result), "%s %c%02d%02d", stamp, offset < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
    xmlSetProp(node, (const xmlChar *)key, (const xmlChar *)result);
    xmlFree(value);
}

char *rs_epg_timezone(const char *xml, size_t len, int offset_minutes, char *err, size_t errlen) {
    if (len > 32 * 1024 * 1024) { snprintf(err, errlen, "EPG exceeds the 32 MB conversion limit."); return NULL; }
    xmlDoc *doc = xmlReadMemory(xml, (int)len, "epg.xml", NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    xmlNode *root = doc ? xmlDocGetRootElement(doc) : NULL;
    if (!root || xmlStrcmp(root->name, (const xmlChar *)"tv")) {
        if (doc) xmlFreeDoc(doc);
        snprintf(err, errlen, "EPG timezone conversion requires an XMLTV document."); return NULL;
    }
    for (xmlNode *node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE || xmlStrcmp(node->name, (const xmlChar *)"programme")) continue;
        convert_time(node, "start", offset_minutes);
        convert_time(node, "stop", offset_minutes);
    }
    xmlChar *output = NULL; int length = 0;
    xmlDocDumpMemoryEnc(doc, &output, &length, "UTF-8");
    char *result = output ? rs_strdup((const char *)output) : NULL;
    xmlFree(output); xmlFreeDoc(doc);
    return result;
}
