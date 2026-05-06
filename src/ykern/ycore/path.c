#include <ykern/ycore/path.h>
#include <ykern/ycore/text.h>

#include <stdlib.h>
#include <string.h>

struct ykern_ycore_text_result ykern_ycore_path_join(const char *parent_path, const char *segment)
{
    if (!segment) {
        return YKERN_ERR(ykern_ycore_text, "ykern_ycore_path_join: segment is NULL");
    }
    if (!parent_path || parent_path[0] == '\0') {
        /* No parent — return "/<segment>". */
        return ykern_ycore_text_format("/%s", segment);
    }

    /* Trim a trailing slash from parent so "/" + child doesn't double up. */
    size_t plen = strlen(parent_path);
    while (plen > 0 && parent_path[plen - 1] == '/') {
        plen--;
    }
    return ykern_ycore_text_format("%.*s/%s", (int)plen, parent_path, segment);
}
