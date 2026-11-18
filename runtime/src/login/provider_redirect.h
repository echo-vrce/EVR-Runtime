// login/provider_redirect.h: makes the game's net-service providers load pnsrad, so login works without -noovr.
#pragma once

namespace evr_provider {

/// Hooks CNSProvider::LoadByName and swaps the "pnsovr" provider name for "pnsdemo" before the game loads it, so
/// login never takes the Oculus path (which needs an Oculus session this install does not have, and otherwise fails
/// with "One or more prerequisites are missing"). The demo provider logs in from the login URL's own credentials,
/// exactly as a -noovr run does. Returns false (and logs) on a prologue mismatch. Installed before the game builds
/// its providers.
bool Install();

}  // namespace evr_provider
