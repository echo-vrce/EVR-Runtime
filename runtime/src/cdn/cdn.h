// cdn/cdn.h: the AtlasCDN client. See cdn_index.h for the wire contract. The index logic (cdn_index.h) is shared by
// PC and Quest; each platform has its own client file (cdn_pc.cpp now, Quest later).
#pragma once

namespace evr_cdn {

/// Starts the worker: fetches the signed index now and every 10 minutes, downloads and checks the files into
/// plugins\cdn\files, and serves each entry's bytes when the game reads its resource. Off when no key is pinned.
void Start();

}  // namespace evr_cdn
