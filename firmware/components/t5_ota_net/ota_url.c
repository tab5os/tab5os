/* SPDX-License-Identifier: Apache-2.0
 * OS updates from a URL: see ota_url.h.
 */
#include "t5_ota/ota.h"
#include "t5_net/download.h"
#include "t5_ota_net/ota_url.h"

t5_err_t t5_ota_install_url(const char *url, t5_ota_source_t from, bool consent, char *err, size_t errlen)
{
    t5_download_t *dl;
    T5_RETURN_ON_ERR(t5_download_open(url, &dl, NULL, err, errlen));
    t5_err_t e = t5_ota_install_stream(t5_download_read, dl, from, consent, NULL, err, errlen);
    t5_download_close(dl);
    return e;
}
