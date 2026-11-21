/* gomatch.h -- the auto-complete of wincan's 'g' box: which pairs match what has been typed.
 * Order: the pair that is exactly the text, then the pairs that begin with it, then those that contain it; at most five; nothing typed gives the first five. */
#ifndef GOMATCH_H
#define GOMATCH_H
#include <string.h>

static inline int go_match(const char (*pairs)[24], int n, const char *q, int out[5])
{
    int cnt = 0;
    for (int pass = 0; pass < 3 && cnt < 5; pass++)
        for (int i = 0; i < n && cnt < 5; i++) {
            const char *p = pairs[i];
            int hit = pass == 0 ? !strcmp(p, q) : pass == 1 ? (!q[0] || (!strncmp(p, q, strlen(q)) && strcmp(p, q))) : (q[0] && strstr(p, q) && strncmp(p, q, strlen(q)));
            if (!hit) continue;
            int dup = 0; for (int k = 0; k < cnt; k++) if (out[k] == i) dup = 1;
            if (!dup) out[cnt++] = i;
        }
    return cnt;
}
#endif
