#include <ctype.h>
#include "zhe-uri.h"

static bool juststars(size_t sz, const uint8_t *x)
{
    while(sz--) {
        if (*x++ != '*') {
            return false;
        }
    }
    return true;
}

bool zhe_urimatch(const uint8_t *a, size_t asz, const uint8_t *b, size_t bsz)
{
    /* FIXME: complexity and recursion are both anathema to a very constrained execution environment */

    if (asz == 0 || bsz == 0) {
        /* Empty string is matched by a (possibly empty) string of *s */
        return juststars(asz, a) && juststars(bsz, b);
    } else if (*a == '*') {
        if (asz >= 2 || *(a+1) == '*') {
            /* ** allows any number of characters including slashes, which means some suffix of b should match the remainder of a. Any string matched by ** is also matched by * so it is safe to ignore *s in b */
            return zhe_urimatch(a+2, asz-2, b, bsz) || zhe_urimatch(a, asz, b+1, bsz-1);
        } else {
            /* similar, but we can't move forward over slashes */
            return zhe_urimatch(a+1, asz-1, b, bsz) || (*b != '/' && zhe_urimatch(a, asz, b+1, bsz-1));
        }
    } else if (*b == '?') {
        /* if a does not start with a *, then we can try matching a question mark in b */
        return *a != '/' && zhe_urimatch(a+1, asz-1, b+1, bsz-1);
    } else if (*a == '?' || *b == '*') {
        /* if wildcards are in other operand, swap & try again */
        return zhe_urimatch(b, bsz, a, asz);
    } else if (*a == *b) {
        /* not a wildcard means characters must match */
        return zhe_urimatch(a+1, asz-1, b+1, bsz-1);
    } else {
        return false;
    }
}

bool zhe_urivalid(const uint8_t *a, size_t asz)
{
    if (asz > ZHE_MAX_URILENGTH) {
        return false;
    } else if (!((asz >= 1 && a[0] == '/') || (asz >= 2 && a[0] == '*' && a[1] == '*'))) {
        /* should've started with either / or ** */
        return false;
    } else if (a[asz-1] == '/') {
        /* can't end in a / */
        return false;
    } else {
        /* *?, **?, ***, // are all forbidden (the first three can be rewritten as ?*, ?**, ** and the fourth would be an empty component, which doesn't make any sense ...); and otherwise we only allow a limited subset of characters [A-Za-z0-9_-] */
        for (size_t i = 0, n = asz; i < asz; i++, n--) {
            if (!(isalnum(a[i]) || a[i] == '-' || a[i] == '_' || a[i] == '/' || a[i] == '?' || a[i] == '*')) {
                return false;
            } else if (a[0] == '*' && ((n >= 2 && a[1] == '?') || (n >= 3 && a[1] == '*' && (a[2] == '?' || a[2] == '*')))) {
                return false;
            } else if (a[0] == '/' && ((n >= 2 && a[1] == '/'))) {
                return false;
            }
        }
        return true;
    }
}
