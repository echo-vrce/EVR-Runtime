// login/login.h: declares EVR-Runtime to Atlas in the game's own login request, as its "evr_runtime" object.
#pragma once

namespace evr_login {

/// Hooks the login-service send so the login profile carries {"evr_runtime":{version,build,commit,platform,channel,
/// social}}. `version` is the loader's version string.
bool Install(const char* version);

}  // namespace evr_login
