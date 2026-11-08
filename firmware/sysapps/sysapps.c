/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_app/apps.h"
#include "sysapps/sysapps.h"
#include "sysapps_priv.h"

t5_err_t t5_sysapps_register(void)
{
    const t5_builtin_app_t *apps[] = { &sysapp_settings, &sysapp_files, &sysapp_appmanager, &sysapp_power,
                                       &sysapp_ports, &sysapp_camera,
                                       &sysapp_terminal, &sysapp_editor };
    for (size_t i = 0; i < sizeof apps / sizeof apps[0]; i++) T5_RETURN_ON_ERR(t5_apps_register_builtin(apps[i]));
    return T5_OK;
}
